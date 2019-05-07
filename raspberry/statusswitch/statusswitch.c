#include <stdint.h>
#include <stdbool.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <time.h>
#include <syslog.h>

#include <wiringPi.h>
#include <wiringPiI2C.h>

#include <mosquitto.h>

#define I2C_ADDR_LEVER	    0x24

const char* MQTT_HOST 	= "platon";
const int   MQTT_PORT 	= 1883;
const char* MQTT_TOPIC_STATE 	= "Netz39/Things/StatusSwitch/Lever/State";
const char* MQTT_TOPIC_EVENTS 	= "Netz39/Things/StatusSwitch/Lever/Events";

#define MQTT_MSG_MAXLEN		  16
const char* MQTT_MSG_LEVEROPEN    = "open";
const char* MQTT_MSG_LEVERCLOSED  = "closed";
const char* MQTT_MSG_LEVERNEUTRAL = "neutral";
const char* MQTT_MSG_LEVERCHANGE  = "lever change";
const char* MQTT_MSG_NONE	  = "none";

struct lever_state_t {
  /*
   * Note: While it does not make logical sense to have the lever both open 
   * and closed, it is technically possible by activating both inputs on 
   * the uC. Therefore we track the states independent from each other.
   */
  bool lever_open;	// Lever is in state open
  bool lever_closed;	// Lever is in state closed
};

/**
 * Get the milliseconds since epoch.
 */
long current_millis() {
  struct timeval tv;
  gettimeofday(&tv, 0);
  
  return tv.tv_sec*1000L + tv.tv_usec/1000L;
}

///// I2C stuff /////

/**
  * This record contains the I2C file descriptors we use in this program
  */
struct I2C_descriptors {
  int lever;
} I2C_fd;

#define I2C_FD_LEVER (I2C_fd.lever)

/**
  * Initialize an I2C channel to the specified address. Exits with an error
  * message if the initialization fails,
  *
  * @param addr The target address.
  * @return File Descriptor for the I2C channel
  */
int I2C_setup_fd(const int addr) {
  const int fd = wiringPiI2CSetup(addr);
  if (!fd) {
    syslog(LOG_EMERG, "Error %d on I2C initialization!", errno);
    exit(-1);
  }
  return fd;
}

/**
  * Initialize all I2C channels and store the file descriptors.
  */
void I2C_init(void) {
  I2C_fd.lever = I2C_setup_fd(I2C_ADDR_LEVER);
}

#define I2C_ERR_INVALIDARGUMENT -2

  union I2C_result {
    unsigned char c[2];
    unsigned short r;
  };


int I2C_command(const int fd, const char command, const char data) {
  // check parameter range
  if ((command < 0) || (command > 0x07))
    return I2C_ERR_INVALIDARGUMENT;
  if ((data < 0) || (data > 0x0f))
    return I2C_ERR_INVALIDARGUMENT;
  
  // TODO check fd
  
    
  // build the I2C data byte
  // arguments have been checked, 
  // this cannot be negative or more than 8 bits
  unsigned char send = (command << 4) + data; 
  
  // calculate the parity
  char v = send;
  char c;
  for (c = 0; v; c++) 
    v &= v-1;
  c &= 1;

  // set parity bit  
  send += (c << 7);
  
  union I2C_result result;
  result.r = 0;

  // maximal number of tries
  int hops=20;

  // try for hops times until the result is not zero
  while (!result.c[0] && --hops) {
    // send command
    result.r = wiringPiI2CReadReg16(fd, send);

    // check for transmission errors: 2nd byte is inverted 1st byte
    const unsigned char c = ~result.c[0];
    if (result.c[1] != c) 
      // if no match, reset the result
      result.r = 0;
  }
  
  if (!hops)
    syslog(LOG_DEBUG, "Giving up transmission!\n");
  
  return result.c[0];
}

///// I3C stuff /////

#define LEVER_CMD_RESET		0x00
#define LEVER_CMD_GETSTATE	0x01
#define LEVER_CMD_SETSTATE	0x02

void I3C_reset_lever() {
  I2C_command(I2C_FD_LEVER, LEVER_CMD_RESET, 0x0);
}

///// Status Lever /////

char lever_getstate() {
  // send the command    
  const char state = I2C_command(I2C_FD_LEVER,
                                 LEVER_CMD_GETSTATE, 0);
  
  // return result
  return state;  
}

void decode_lever_state(uint8_t state,
                        struct lever_state_t *ls)
{
  // see http://www.netz39.de/wiki/projects:2014:gatekeeper
  ls->lever_open   = (state & 0x02);
  ls->lever_closed = (state & 0x01);
}                        
                        

int main(int argc, char *argv[]) {
  // initialize the system logging
  openlog("statusswitch", LOG_CONS | LOG_PID, LOG_USER);
  syslog(LOG_INFO, "Starting statusswitch observer.");

  // initialize I2C
  I2C_init();
  
  // initialize MQTT
  mosquitto_lib_init();
  
  void *mqtt_obj;
  struct mosquitto *mosq;
  mosq = mosquitto_new("statusswitch", true, mqtt_obj);
  if ((int)mosq == ENOMEM) {
    syslog(LOG_ERR, "Not enough memory to create a new mosquitto session.");
    mosq = NULL;
  }
  if ((int)mosq == EINVAL) {
    syslog(LOG_ERR, "Invalid values for creating mosquitto session.");
    return -1;
  }
  
  if (mosq) {
    int ret;
    
    ret = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 30);
    if (ret == MOSQ_ERR_SUCCESS)
      syslog(LOG_INFO, "MQTT connection to %s established.", MQTT_HOST);
      
    // TODO error handling
  }
  
  char mqtt_payload[MQTT_MSG_MAXLEN];
  
  // the known lever status
  struct lever_state_t before;
  decode_lever_state(lever_getstate(), &before);
  
  char run=1;
  int i=0;
  while(run) {
    printf("****** %u\n", i++);

    uint8_t status = lever_getstate();
    struct lever_state_t ls;
    decode_lever_state(status, &ls);
    
    printf("Lever status byte: 0x%02x\n", status);
    printf("Open:\t%s\n", (ls.lever_open ? "yes" : "no"));
    printf("Closed:\t%s\n", (ls.lever_closed ? "yes" : "no"));
    printf("\n");

    // Check door status for changes and emit MQTT messages
    mqtt_payload[0] = 0;

    // lever close state changed
    if (before.lever_closed != ls.lever_closed) {
      if (ls.lever_closed && !ls.lever_open) {
        syslog(LOG_INFO, "Lever has been switched to closed.");
        //MQTT message
        strcpy(mqtt_payload, MQTT_MSG_LEVERCLOSED);
      } else if (ls.lever_closed == ls.lever_open) {
        syslog(LOG_INFO, "Lever has been switched to neutral state.");
        //MQTT message
        strcpy(mqtt_payload, MQTT_MSG_LEVERNEUTRAL);      
      }
      
      before.lever_closed = ls.lever_closed;
    }

    // lever open state changed
    if (before.lever_open != ls.lever_open) {
      if (ls.lever_open && !ls.lever_closed) {
        syslog(LOG_INFO, "Lever has been switched to open.");
        //MQTT message
        strcpy(mqtt_payload, MQTT_MSG_LEVEROPEN);
      } else if (ls.lever_closed == ls.lever_open) {
        syslog(LOG_INFO, "Lever has been switched to neutral state.");
        //MQTT message
        strcpy(mqtt_payload, MQTT_MSG_LEVERNEUTRAL);      
      }
      
      before.lever_open = ls.lever_open;
    }

    // send MQTT messages if there is payload
    if (mqtt_payload[0] && mosq) {
      int ret;
      int mid;
      // state message
      ret = mosquitto_publish(
                        mosq, 
                        &mid,
                        MQTT_TOPIC_STATE,
                        strlen(mqtt_payload), mqtt_payload,
                        2, /* qos */
                        true /* retain */
                       );
      if (ret != MOSQ_ERR_SUCCESS)
        syslog(LOG_ERR, "MQTT error on message \"%s\": %d (%s)", 
                        mqtt_payload, 
                        ret,
                        mosquitto_strerror(ret));
      else
        syslog(LOG_INFO, "MQTT message \"%s\" sent with id %d.", 
                         mqtt_payload, mid);
                         
      // change event
      strcpy(mqtt_payload, MQTT_MSG_LEVERCHANGE);
      ret = mosquitto_publish(
                        mosq, 
                        &mid,
                        MQTT_TOPIC_EVENTS,
                        strlen(mqtt_payload), mqtt_payload,
                        2, /* qos */
                        false /* don't retain */
                       );
      if (ret != MOSQ_ERR_SUCCESS)
        syslog(LOG_ERR, "MQTT error on message \"%s\": %d (%s)", 
                        mqtt_payload, 
                        ret,
                        mosquitto_strerror(ret));
      else
        syslog(LOG_INFO, "MQTT message \"%s\" sent with id %d.", 
                         mqtt_payload, mid);
      
    }
    
    // call the mosquitto loop to process messages
    if (mosq) {
      int ret;
      ret = mosquitto_loop(mosq, 100, 1);
      // if failed, try to reconnect
      if (ret)
        mosquitto_reconnect(mosq);
    }

    I3C_reset_lever();
    
    if (sleep(1)) 
      break;
  }

  // clean-up MQTT
  if (mosq) {
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
  }
  mosquitto_lib_cleanup();

  syslog(LOG_INFO, "Doorstate observer finished.");
  closelog();
    
  return 0;
}
