/*
 * Modem_BGxx.h
 *
 *  Created on: Nov 14, 2018
 *      Author: jsilva
 */

#ifndef INC_MODEM_BGXX_H_
#define INC_MODEM_BGXX_H_


#include <timestamp.h>
#include "malloc.h"
#include "string"
#include "strstream" // to use string import this
#include <algorithm>
#include "queue"

#include "package.h"
#include "aux_func.h"
#include "settings.h"
#include "logging.h"
#include "modem.h"
#include "flash.h"
#include "md5.h"


using namespace std;

struct Modem_state{
  bool configured;
  char imei[16];
  int8_t rssi;
  char radio[5];
};

#define MAX_SMS 10

struct SMS {
	bool    used   	= false;
	uint8_t index  	= 0;
	char origin[20] = "";
	char msg[256]    = "";
};


struct st_machine{
		uint8_t previous_state;
		uint8_t actual_state;
		uint8_t event;
		uint32_t timeout;
		uint8_t tries;
};

struct CID{
  uint8_t id; /* id */
	uint8_t socket; /* port */
  char ip[15];
};

// CONSTANTS
#define   AT_WAIT_RESPONSE      10 // milis
#define   MAX_CONNECTIONS       2
#define   BUFFER_SIZE         	256 // bytes
#define   CONNECTION_STATE    	2000 // millis // where I use it?
#define 	MAX_TIME_TO_CONNECT		50000 // millis

#define 	MODEM_STATE_UNKNOW							0
#define 	MODEM_STATE_CONNECTED						1
#define 	MODEM_STATE_DISCONNECTED				2 // shouldn't be used
#define 	MODEM_STATE_SAVEMODE		 				3
#define 	MODEM_STATE_CONNECTING					4
#define 	MODEM_STATE_PSM   							5
#define 	MODEM_STATE_SETUP								6
#define 	MODEM_STATE_SHUTDOWN       			12

#define 	MODEM_EVENT_UNKNOW							0
#define 	MODEM_EVENT_CONNECT							1
#define 	MODEM_EVENT_DISCONNECT					2
#define 	MODEM_EVENT_SWITCH_OFF_RADIO	  3
#define 	MODEM_EVENT_SLEEP            	  4
#define 	MODEM_EVENT_SHUTDOWN         	  6

#define MAX_N_ACTIVE_CONTEXTS		1
#define MAX_N_PDP_CONTEXTS		10


#ifdef __cplusplus
 extern "C" {
#endif

class Modem_BGxx {
	public:

		// constructor
		// 1st arg -> serial for log messages
		// 2nd arg -> serial for communication with modem
		Modem_BGxx(uint8_t usart, uint16_t buadrate, int8_t gpio_reset); //OK

    void restart();

		uint8_t prev_state();
		uint8_t state();
		void state_machine();
		bool call_event(uint8_t event);

		bool check_data_received(uint8_t *index, uint16_t* size);
		uint16_t recv(uint8_t index, char* data, uint16_t size); // call to get data from buffers

		bool ready(); // check if modem is ready (if it's listening for AT commands)
		bool reset(); //sw reboot
    bool reset_hw(); // reset modem power (turn off and then back on)
		bool switch_off_radio();
    bool sleeping(); // change state to sleep mode

    bool config(); // configure base settings like ECHO mode and multiplex
    bool disableAllPeripherals(); // OK

    bool connected(); 		// check if a cid is connected
    string connected_radio(); 		// get current radio in use
		bool setup();         // setup APN configuration
		bool join();          // join an APN
    bool setPSM(uint32_t t_tau, uint16_t t_active); // set Power Saving Mode
    bool close_pdp_context(); //OK
    bool disable_pdp(); //OK

		// implemet
		// unjoin from apn -> cid (AT+CEREG=0) ??

		// implement
		// choose what cid is on

    // --- SMS ---
    void sms_delete_msgs();
    SMS* sms_get_next_msg();
    bool sms_has_msg();
    void sms_check();
    bool sms_send(string origin, string msg);
    bool sms_remove_all();
    bool sms_remove(uint8_t index);

    // --- UDP ---
    /*
		bool openSocket(uint8_t *socket, uint16_t port, uint16_t timeout = 5000); // open socket
		bool openSocket(uint8_t socket, uint16_t port, uint16_t timeout = 5000); // open socket

    // --- UDP --- Old code
		bool send_udp(uint16_t socket, string ipaddress, uint16_t port, char* data_c, uint16_t size); //OK
		bool send_udp(uint16_t socket, string ipaddress, uint16_t port, char* data_c, uint16_t size, uint8_t tries); //OK
		uint16_t read_udp_socket(uint16_t socket, uint16_t size); //OK
    */

    void check_signal();
    int16_t rssi(); //OK
    bool sync_clock_ntp();
		bool getTimeZone(tm* curr_time);

		bool parseResponse(string command); //OK

    string getImei(); // OK
    string getSimCardId();
    string getIP(); //OK
    string getVersion();
    string getLocation();
    void log_info();

    // --- TCP/UDP ---
    bool open(uint8_t context_id, uint8_t connect_id, string protocol, string host, uint16_t port); // open
    bool close(uint8_t connect_id); //close socket
    bool send(uint8_t connect_id, string data, uint16_t length);
    uint16_t getData(uint8_t connect_id, uint16_t length);
    uint16_t readData(uint8_t* data, uint16_t length);
    string getNewLine(uint8_t* data, uint16_t length);

    // --- HTTP ---
    // get
    bool HTTP_get_fota(string host,uint16_t port,string address);
    uint16_t HTTP_get_config(uint8_t* body, string host,string address);
    bool parseHeader(string header, uint16_t header_len);
    bool update(string host,uint16_t port,string address);

    // --- MQTT ---
    void MQTT_init(void(*callback)(string));
    bool MQTT_setup(uint8_t client, string will_topic);
    bool MQTT_open(uint8_t client);
    bool MQTT_isOpened(uint8_t client);
    int8_t MQTT_close(uint8_t client);
    int8_t MQTT_connect(uint8_t client);
    bool MQTT_connected(uint8_t client);
    int8_t MQTT_checkConnection(uint8_t client);
    int8_t MQTT_disconnect(uint8_t client);
    bool MQTT_subscribeTopic(uint8_t client, uint16_t msg_id, string topic,uint8_t qos, uint8_t len);
    bool MQTT_subscribeTopics(uint8_t client, uint16_t msg_id, string topic[],uint8_t qos[], uint8_t len);
    int8_t MQTT_unSubscribeTopic(uint8_t client, uint16_t msg_id, string topic[], uint8_t len);
    int8_t MQTT_publish(uint8_t client, uint16_t msg_id,uint8_t qos, uint8_t retain, string topic, string msg, bool ts = false);
    string MQTT_readMessages(uint8_t client);

    // --- variables ---
		uint8_t gpio_reset;

	private:

		// data buffer for each connection
		char buffers[MAX_CONNECTIONS][BUFFER_SIZE];

		// size of each buffer
		uint16_t  buffer_len[MAX_CONNECTIONS];
    // connection status
    uint8_t connected_state[MAX_CONNECTIONS];
    // size of each datagram

    bool mqtt_connected = false;

    // pending messages
		SMS sms[MAX_SMS];
    uint8_t counter = 0; // msg queue counter
    uint8_t tail = 0; // msg queue old message

    char last_origin[20]; // last number sending sms
    /*
		// connection state of each connection
		bool connected_state[MAX_CONNECTIONS];

		// validity of each connection state
		uint32_t connected_until[MAX_CONNECTIONS];
    */

		// logging
		void log(uint8_t level,string text);
		void log(uint8_t level,const char* text);
		const char* print_state(uint8_t state);
		const char* print_event(uint8_t event);

    bool change_state(uint8_t state);

};

extern Modem_BGxx* modem;

#ifdef __cplusplus
}
#endif


#endif /* INC_MODEM_BGXX_H_ */
