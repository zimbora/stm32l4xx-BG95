/*
 * Modem_BGxx.cpp
 *
 *  Created on: Nov 14, 2018
 *      Author: jsilva
 */



#include <Modem_BGxx.h>



//extern LOGGING logging;

/*
	call the next functions by the respective order
	Modem_BGxx()
	state_machine
	check_data_received
*/


Modem_BGxx* modem = NULL;

struct Modem_state dev = {
	false,
	"", /* imei */
	0, /* rssi */
	"", /*radio*/
};

struct CID cid = {
	/* id     		*/ 1,
	/* socket 		*/ 0,
	/* ip 				*/ "",
};


struct st_machine st = {
  /* previous_state */  MODEM_STATE_UNKNOW,
	/* actual_state */ MODEM_STATE_SHUTDOWN,
	/* event */ 			MODEM_EVENT_UNKNOW,			// success changing state
	/* timeout */       0,
	/* tries */       0
};

int8_t mqtt_buffer[] = {-1,-1,-1,-1,-1}; // index of msg to read

MODEM *bgxx = NULL;
void (*parseMQTTmessage)(string);

uint8_t tries = 0;
uint8_t mqtt_tries = 0;
int8_t mqtt_client = 0;

uint32_t content_length = 0;
string md5 = "";
MD5_CTX mdctx;


Modem_BGxx::Modem_BGxx(uint8_t usart, uint16_t buadrate, int8_t gpio_reset) {

	reset_hw(); // turn on

	if(bgxx == NULL)
  	bgxx = new MODEM();

	uint8_t i = 0;
	while(i<MAX_CONNECTIONS){
		buffer_len[i] = 0;
		i++;
	}

}


void Modem_BGxx::restart() {
	bgxx->restart();
}


void Modem_BGxx::state_machine() {

	if(state() == MODEM_STATE_SHUTDOWN){
		return;
	}

	if(state() == MODEM_STATE_UNKNOW){
		//set_rgb_colour(BLUE);
		log(2,"Detecting modem..");
		if(ready())
			change_state(MODEM_STATE_SETUP); // call it only once // call it only once
		else return;
	}

	if(state() == MODEM_STATE_SETUP){

		// call it only once
		if(!ready()) return;

		if(!config()) return;

		/*
		// unsolicited code
		if (bgxx->checkAtCommand("AT+CSCON=1", "OK",100))
			log(2,"cscon disabled");
		*/

		/*
		if (bgxx->checkAtCommand("AT+NPSMR=1", "OK",100))
			log(2,"PSM report set");
		*/

		//power saving mode
		#ifdef PSMMODE
			setPSM(settings.modem.tau, settings.modem.awake);
		#else
			bgxx->checkAtCommand("AT+CPSMS=0", "OK",1000);
		#endif

		if (!bgxx->checkAtCommand("AT+CFUN=1", "OK", 3000)) return;

		//getSimCardId();
		getImei();

		if(!bgxx->checkAtCommand("AT+CIMI", "OK", 3000))
			logging.log("!! sim card not inserted","BGxx");

		if(!setup()) return;

		join();

		change_state(MODEM_STATE_CONNECTING);
	}

	if(state() == MODEM_STATE_CONNECTING){
		rssi();
		getIP();
		string rsp = bgxx->getAtCommandResponse("AT+QIACT?","+QIACT: 1,",300);
		int8_t index = find(rsp,",");
		if(index > -1){
			rsp = rsp.substr(0,index);
			if(has_only_digits(rsp)){
				uint8_t state = (uint8_t)std::stoul(rsp);
				if(state == 1){
					change_state(MODEM_STATE_CONNECTED);
					getIP();
					log(3,"Modem has IP");
				}
			}
		}
	}

	if(state() == MODEM_STATE_CONNECTED){

		if(getTimestamp() < 1654700400)
			sync_clock_ntp();

		//log(3,"Modem connected");
		//set_rgb_colour(GREEN);
		//bgxx->checkAtCommand("AT","OK",2000);
		bgxx->checkMessages();
	}

	if(state() == MODEM_STATE_PSM){
		log(3,"Modem in PSM mode");
		//log("Modem is sleeping");
		bgxx->checkMessages();
	}

  sms_check();

	check_signal();

	return;
}

uint8_t Modem_BGxx::prev_state(){
	return st.previous_state;
}

uint8_t Modem_BGxx::state(){
	return st.actual_state;
}

bool Modem_BGxx::change_state(uint8_t state){
	st.previous_state = st.actual_state;
	st.actual_state = state;
	return true;
}

bool Modem_BGxx::call_event(uint8_t event){

	bool result = false;
	//st.actual_state = st.prev_state;
	if(event == MODEM_EVENT_CONNECT){
		if(state() == MODEM_STATE_PSM){
			return false;
			//st.timeout = millis()+MAX_TIME_TO_CONNECT;
		}else if(state() != MODEM_STATE_CONNECTED){
			result = true;
			change_state(MODEM_STATE_SETUP);
			//st.timeout = millis()+MAX_TIME_TO_CONNECT;
		}else log(2,"bgxx is allready connected");
	}else if(event == MODEM_EVENT_SWITCH_OFF_RADIO){
		switch_off_radio();
		result = true;
	}else if(event == MODEM_EVENT_DISCONNECT){
		if(close(cid.socket)){
			result = true;
			change_state(MODEM_STATE_DISCONNECTED);
			log(2,"bgxx is disconnected");
		}
		close_pdp_context();
	}else if(event == MODEM_EVENT_SLEEP){
		#ifndef PSMMODE
		if(close(cid.socket)){
			result = true;
			//change_state(MODEM_STATE_PSM);
			log(2,"bgxx socket was closed");
		}
		#endif
		// close socket !!!
	}else if(event == MODEM_EVENT_SHUTDOWN){
		change_state(MODEM_STATE_SHUTDOWN);
		log(3,"bgxx was switched off");
		// close socket !!!
	}
	log_info();

	return result;
}

/* return true if there is data available in the buffers
 * index of the buffer and respective size is stored in the pointers
 * passed as arguments
 */
bool Modem_BGxx::check_data_received(uint8_t *index, uint16_t* size){

	uint8_t i = 0;
	for(i = 0; i < MAX_CONNECTIONS; i++){
		*index = i;
		*size = 0;
		if (buffer_len[i] > 0) {
				*size = buffer_len[i];
			#ifdef DEBUG_MODEM_MESSAGE
				sprintf(logging.log_msg,"buffer with index: %d has %d bytes to be read",i,*size);
				logging.log(logging.log_msg,"bc68");
			#endif
			return true;
		}
	}
	return false;
}

/* cpy data from buffer with respective index to data pointer
*  This function should be called with some frequency
*
*  index -> sock to read
*  data -> buffer to cpy data read
*  size -> number of bytes to read
*  return number of bytes read
*/
uint16_t Modem_BGxx::recv(uint8_t index, char* data, uint16_t size) {
	if (buffer_len[index] == 0 || size > buffer_len[index]) return 0;

	uint16_t i;

	if (buffer_len[index] <= size) {
		size = buffer_len[index];

		for (i = 0; i < size; i++) {
			data[i] = buffers[index][i];
		}

		// it should not happen
		buffer_len[index] = 0;

		return size;
	}

	for (i = 0; i < size; i++) {
		data[i] = buffers[index][i];
	}

	for (i = size; i < buffer_len[index]; i++) {
		buffers[index][i - size] = buffers[index][i];
	}

	buffer_len[index] -= size;
	//buffer_pop();

	return size;
}

// check if bgxx is ready
bool Modem_BGxx::ready() {

	if(tries == 2){
		restart();
	}

	if(tries > 2){
		reset_hw();
		tries = 0;
	}else tries++;


	HAL_Delay(200);

	if (bgxx->checkAtCommand("AT", "OK", 1000)) {
		log(1,"AT ok");
		tries = 0;
		return true;
	}

	log(2,"AT not ok");
	return false;
}

/*
args:
	t_tau in Minutes
	t_active in Seconds with a maximum of 32 minutes
	!! something is being badly calculated on tau parameter
*/
bool Modem_BGxx::setPSM(uint32_t t_tau, uint16_t t_active){
	uint8_t value_active = 0, value_tau = 0;
	uint8_t time = 0;
	uint8_t scale = (uint8_t)(t_active / 60);

	/*
	sprintf(logging.log_msg,"scale: %d",scale);
	logging.log(logging.log_msg,"bc68");
	*/

	if(scale == 0){
		time = (uint8_t)(t_active % 60);
		value_active = 0x00 | (time/2);
	}else if(scale > 0 && scale < 32){
		time = scale;
		value_active = 0b00100000 | time;
	}else return false;


	/*
	float a = (float)t_active;
	sprintf(logging.log_msg,"t_active: %f",a);
	logging.log(logging.log_msg,"bc68");
	sprintf(logging.log_msg,"value_active: %f",remainder(a,30));
	logging.log(logging.log_msg,"bc68");
	sprintf(logging.log_msg,"value_active: %d",value_active);
	logging.log(logging.log_msg,"bc68");
	*/

	scale = (uint8_t)(t_tau / 10);
	if(scale < 6)
		value_tau = 0x00 | scale;

	scale = (uint8_t)(t_tau / 60);
	if(scale > 0 && scale < 32)
		value_tau = 0b00100000 | scale;

	scale = (uint8_t)(t_tau / (60*30)); // if time > 30 hours
	if(scale > 0 && scale < 32)
		value_tau = 0b01000000 | scale;

	scale = (uint8_t)(t_tau / (60*320)); // if time > 320 hours
	if(scale > 0 && scale < 32)
		value_tau = 0b11000000 | scale;


	sprintf(logging.log_msg,"value_tau: %d",value_tau);
	logging.log(logging.log_msg,"bc68");


	/*
	It will be 0 if tau lower than 10 minutes
	if(value_tau == 0)
		return false;
	*/

	string tau = "", active = "";
	uint8_t i = 8;
	while(i > 0){
		if(value_active & 0x80)
			active.append("1");
		else active.append("0");
		value_active = value_active << 1;
		if(value_tau & 0x80)
			tau.append("1");
		else tau.append("0");
		value_tau = value_tau << 1;
		i--;
	}

	string s = "AT+CPSMS=1,,,\""+tau+"\",\""+active+"\"";
	if(!bgxx->checkAtCommand(s.c_str(), "OK", 200))
		return false;

	//change_state(MODEM_STATE_PSM);

	return true;
}

// reset bgxx
bool Modem_BGxx::reset() {

	log(4,"reseting bgxx");
	cid.socket = 0;
	memset(cid.ip,0,sizeof(cid.ip));
	//memcpy(cid.ip,"",15);
	bgxx->sendAtCommand("AT+NRB");

	dev.configured = false;
	change_state(MODEM_STATE_UNKNOW);

	return true;
}


// This function only switches off the radio
bool Modem_BGxx::switch_off_radio() {
	cid.socket = 0;
	memset(cid.ip,0,sizeof(cid.ip));
	//memcpy(cid.ip,"",15);
	if(bgxx->checkAtCommand("AT+CFUN=0", "OK", 3000))
		return true;
	return false;
}

// reset bgxx through hardware
bool Modem_BGxx::reset_hw() {
	cid.socket = 0;
	memset(cid.ip,0,sizeof(cid.ip));
	//memcpy(cid.ip,"",15);
	log(4,"reseting bgxx");

	// Switch On/Off Modem
	HAL_Delay(500);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
	HAL_Delay(500);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
	HAL_Delay(2000);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);

	HAL_Delay(4000);

	dev.configured = false;
	change_state(MODEM_STATE_UNKNOW);
	return true;

}

// not working
// private function
string Modem_BGxx::getSimCardId(){

	string response = "";
	/*
	response = bgxx->getAtCommandResponse("AT+NCCID","+NCCID:",1000);
	log(3,"ccid: "+response);
	*/
	return response;
}

string Modem_BGxx::getImei(){
	if(strcmp(dev.imei,"") != 0)
		return "imei:"+string(dev.imei);

	string response = "";
	response = bgxx->getAtCommandResponse("AT+CGSN","",1000);

	log(3,"imei: "+response);
	memset(dev.imei,0,16);
	memcpy(dev.imei,response.c_str(),15);

	if(!has_only_digits(string(dev.imei)))
		return "";

	return "imei:"+string(dev.imei);
}

bool Modem_BGxx::sync_clock_ntp(){

	string response = bgxx->getAtCommandResponseNoOK("AT+QNTP=1,\"pool.ntp.org\",123","+QNTP: ", 15000);
	return response.length() > 0;
}
// format: 19/03/08,11:50:25+00
bool Modem_BGxx::getTimeZone(tm* curr_time){

	string response = "";
	string aux = "";
	response = bgxx->getAtCommandResponse("AT+CCLK?","+CCLK:",1000);

	response.erase(std::remove(response.begin(), response.end(), '\"'), response.end());
	response.erase(std::remove(response.begin(), response.end(), ' '), response.end());
	/*
	[modem] << +CCLK:19/10/01,18:09:11+04OK
	[tasks]: timestamp 1543687751
	*/

	int index = find(response,"/");
	if(index > 0){
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		int year = stoi(aux,nullptr,10);
		if(year > 79)
			return false;
		curr_time->tm_year = year;
		response = response.substr(index+1,response.length());
		index = find(response,"/");
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		curr_time->tm_mon = stoi(aux,nullptr,10);
		response = response.substr(index+1,response.length());
		index = find(response,",");
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		curr_time->tm_mday = stoi(aux,nullptr,10);
		response = response.substr(index+1,response.length());
		index = find(response,":");
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		curr_time->tm_hour = stoi(aux,nullptr,10);
		response = response.substr(index+1,response.length());
		index = find(response,":");
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		curr_time->tm_min  = stoi(aux,nullptr,10);
		response = response.substr(index+1,response.length());
		index = find(response,"+");
		aux = response.substr(0,index);
		if(!has_only_digits(aux))
			return false;
		curr_time->tm_sec  = stoi(aux,nullptr,10);
	}else return false;

	return true;
}

// not available for this modem
bool Modem_BGxx::disableAllPeripherals(){
	return true;
}

// disable echo, get imei and config sim card
bool Modem_BGxx::config() {
	uint8_t flag_reset = false;

	if(!bgxx->checkAtCommand("AT","OK",3000)) return false;

	if(dev.configured)
		return true;

	bgxx->checkAtCommand("AT&F", "OK", 1000);

	bgxx->checkAtCommand("ATE0", "OK",1000); //disable echo

	getVersion();

	dev.configured = true;

	//bgxx->checkAtCommand("AT+QREGSWT=2", "OK",1000); //disable automatic registration

	if(flag_reset){
		log(4,"Resetting bgxx");
		reset();
		HAL_Delay(2000);
		return false;
	}

	return true;
}

bool Modem_BGxx::setup() {

	string response = "";
	string s = "";
	string pdp_type = "IP";
	// SET APN
	HAL_Delay(1000);

	//Set SMS message format as text mode
	if (!bgxx->checkAtCommand("AT+CMGF=1", "OK", 1000)) return false;

	// Set character set as GSM which is used by the TE
	//if (!bgxx->checkAtCommand("AT+CSCS=\"GSM\"", "OK", 1000)) return false;
	if (!bgxx->checkAtCommand("AT+CSCS=\"IRA\"", "OK", 1000)) return false;

	bgxx->checkAtCommand("AT+CEREG?", "OK", 2000);
	// Enable GPS
	//bgxx->checkAtCommand("AT+QGPS=1", "OK", 400);

	bgxx->getAtCommandResponse("AT+COPS?", 300);

	if(settings.modem.radio == GPRS){
		s = "AT+QICSGP="+to_string(cid.id)+",1,\"" + settings.gprs.apn + "\",\"" + settings.gprs.user + "\",\"" + settings.gprs.pwd + "\"";
		bgxx->checkAtCommand(s.c_str(), "OK", 500);  // replacing CGDCONT
		/*
		s = "AT+QCFG=\"nwscanmode\",1,1";
		if(!bgxx->checkAtCommand(s.c_str(), "OK",100)) return false;
		*/
		s = "AT+COPS=4,2,\""+to_string(settings.gprs.cops)+"\","+to_string(0);
	}else if(settings.modem.radio == NB){
		s = "AT+QICSGP="+to_string(cid.id)+",1,\"" + settings.nb.apn + "\",\"" + settings.nb.user + "\",\"" + settings.nb.pwd + "\"";
		bgxx->checkAtCommand(s.c_str(), "OK", 500);  // replacing CGDCONT
		/*
		s = "AT+QCFG=\"iotopmode\",1,1";
		if(!bgxx->checkAtCommand(s.c_str(), "OK",100)) return false;

		s = "AT+QCFG=\"nwscanmode\",3,1";
		if(!bgxx->checkAtCommand(s.c_str(), "OK",100)) return false;
		*/
		s = "AT+COPS=4,2,\""+to_string(settings.nb.cops)+"\","+to_string(9);
	}else{

		s = "AT+QICSGP="+to_string(cid.id)+",1,\"" + settings.gprs.apn + "\",\"" + settings.gprs.user + "\",\"" + settings.gprs.pwd + "\"";
		bgxx->checkAtCommand(s.c_str(), "OK", 500);  // replacing CGDCONT

		s = "AT+COPS=4,2,\""+to_string(settings.gprs.cops)+"\"";
	}

	if (!bgxx->checkAtCommand(s.c_str(), "OK",15000)){
		logging.log("!! No network found, check antenna or sim card activation","BGxx");

		/*
		if(ready())
			sms_send("910324016","I am looking for network");
		*/
		//return false;
	}

	//26806 -> pt
	bgxx->getAtCommandResponse("AT+COPS?", 300);

	log(3,"cops set");

	//dev.is_ready = true;
	return true;
}

// not needed
bool Modem_BGxx::join() {

	// ATTACH
	/*

	//if(bgxx->getAtCommandResponse("AT+CGATT?","+CGATT:",1000)!="1") return false; // correct it
	//if (!bgxx->checkAtCommand("AT+CGATT=1", "OK", 10000)) return false;

	cid.id = c_id;
	//string s = "AT+CGACT=1,"+to_string(cid.id);
	*/

	close_pdp_context();

	string s = "AT+QIACT=1";//,"+to_string(cid.id);
	if (!bgxx->checkAtCommand(s.c_str(), "OK", 5000)) return false;


	// check state, if not connected enable cereg ??
	//HAL_Delay(3000);

	bgxx->checkAtCommand("AT+CSQ", "OK", 2000);
	bgxx->checkAtCommand("AT+CEREG?", "OK", 2000);
	//bgxx->checkAtCommand("AT+CSCON?",",1",2000);

	return true;
}


// check if response matches one of the predifined responses
// a flag should be launched in this case
bool Modem_BGxx::parseResponse(string response){

	if(response.size() == 0)
		return false;

	if(DEBUG_COMMANDS <=  VERBOSE_DEBUG_LEVEL)
 	  logging.println("command","<< ",response);

	// unsolicited responses
	if(find(response,"+CEREG") > -1){
		if(find(response,":1") > -1)
		log(3,"what it means?");
		else if(find(response,",:2") > -1)
		log(3,"attaching");
		else if(find(response,":5") > -1)
		log(3,"device is connected");
	}

	else if(find(response,"+CSCON") > -1){
		if(find(response,",1") > -1)
		log(3,"attached");
		else if(find(response,",0") > -1){
			log(3,"dettached");
			log(3,"time started..");
		}
	}

	else if(find(response,"+NPING") > -1)
	log(3,"\n NPING RECEIVED -> Ping");

	/*
	else if(find(response,"+NSONMI") > -1){
		log(3,"\n NSONMI RECEIVED -> Data available on socket\n");
		uint16_t socket = 0;
		string socket_s = "";
		uint16_t len = 0;
		string len_s = "";

		// parse verified
		response = response.substr(find(response,"+NSONMI:")+8);
		log(2,response);
		if((find(response,",")) > -1){
			socket_s = (response.substr(0,find(response,","))); //.toInt();
			socket = stoi(socket_s,nullptr,10);
			len_s = (response.substr(find(response,",")+1)); //.toInt();
			if(len_s.size() > 0)
			len = stoi(len_s,nullptr,10);
			if(len == 0)
			len = 255;
		}

		//while(len > 0) len =
		read_udp_socket(socket, len); // store on buffers
	}

	else if(find(response,"+NPSMR:1") > -1){
		log(3,"\n NPSMR RECEIVED -> Modem is now sleeping\n");
		change_state(MODEM_STATE_PSM);
	}

	else if(find(response,"+NNMI") >-1)
		log(3,"\n NNMI RECEIVED -> New message indicator");

	else if(find(response,"+NSMI") >-1)
		log(3,"\n NSMI RECEIVED -> Sent message indicator");
	*/
	else if(find(response,"+QIURC") >-1){
		if(find(response,"\"pdpdeact\",") >-1){
			response = response.substr(find(response,"\"pdpdeact\",")+11);
			log(3,"\n Context disabled");
			if(response.size() > 0){
				if(isdigit(response.c_str()[0])){
					change_state(MODEM_STATE_DISCONNECTED);
				}
			}
		}
	}

	else if(find(response,"+QCSQ: \"NOSERVICE\"") > -1){
		log(3,"\n No service available");
		change_state(MODEM_STATE_DISCONNECTED);
	}

	else if(find(response,"+QMTSTAT") > -1){
		if(find(response,",1") >-1){
			mqtt_connected = false;
			log(3,"\n MQTT closed");
			//disable_pdp();
			//call_event(MODEM_EVENT_CONNECT);
		}
	}

	else{

			string filter = "+QMTRECV: "+to_string(cid.id)+",";
			int index = find(response,filter);

			if(index > -1){ // filter found
				string aux = response.substr(filter.size());
				if(find(aux,",") > -1) // has payload
					parseMQTTmessage(response.substr(index+filter.size()-2));
				else{ // URC unsolicitated
					aux = aux.substr(0,1);
					if(has_only_digits(aux)){
						uint8_t channel = (uint8_t)stoi(aux);
						logging.println("QMTRECV","channel: ",channel);
						if(channel < 5)
							mqtt_buffer[channel] = 0; // I do not know the length of the payload that will be read
					}
				}
			}
	}

	return true;
}

void Modem_BGxx::sms_delete_msgs() {

	/*
	uint8_t i = 0;
	while(i<counter)
		sms_remove(sms[i++].index);
	*/
	if(counter != 0)
		bgxx->checkAtCommand("AT+CMGD=1,4", "OK", 1000); // delete all messages

	tail = 0;
	counter = 0;
}

SMS* Modem_BGxx::sms_get_next_msg() {
	return &sms[tail++];
}

bool Modem_BGxx::sms_has_msg() {
	if(tail != counter)
		return true;
	else return false;
}

void Modem_BGxx::sms_check() {
	int16_t index = 0;
	string msg_id="",state="",sender="",nd="",time="",msg="";
	bool new_message = false;
	//send_command("AT+CMGL=\"ALL\"");
	string res = bgxx->getAtCommandResponseSMS("AT+CMGL=\"ALL\"","+CMGL: ",500);
	//string res = bgxx->getAtCommandResponse("AT+CMGL=\"REC READ\"","+CMGL: ",500);
	//string res = bgxx->getAtCommandResponseNoOK("AT+CMGL=\"REC UNREAD\"","+CMGL: ",500); // datasheet says max response time is 300ms
	if(res.size() > 0)
		logging.println("sms","rcv: ",res);

	do{
		if(counter == MAX_SMS){
			log(4,"msg queue is full");
			break;
		}

		index = find(res,",");
		if(index == -1) return;

		msg_id = res.substr(0,index);
		logging.println("sms","msg_id: ",msg_id);
		if(!has_only_digits(msg_id))
			break;

		sms[counter].index = stoi(msg_id);
		res = res.substr(index+1);

		index = find(res,",");
		if(index == -1) return;
		state = res.substr(0,index);
		logging.println("sms","state: ",state);
		res = res.substr(index+1);

		index = find(res,",");
		if(index == -1) return;
		sender = res.substr(0,index);
		logging.println("sms","sender: ",sender);
		memset(sms[counter].origin,0,20);
		memcpy(sms[counter].origin,sender.c_str(),sender.size());
		memset(last_origin,0,20);
		memcpy(last_origin,sender.c_str(),sender.size());
		res = res.substr(index+1);

		index = find(res,",");
		if(index == -1) return;
		nd = res.substr(0,index);
		logging.println("sms","nd: ",nd);
		res = res.substr(index+1);

		index = find(res,"\n");
		if(index == -1) return;
		time = res.substr(0,index);
		logging.println("sms","time: ",time);
		res = res.substr(index+1);

		index = find(res,"+CMGL: ");
		if(index != -1){
			new_message = true;
			msg = res.substr(0,index);
			res = res.substr(index+7);
		}else{
			new_message = false;
			msg = res;
		}
		memset(sms[counter].msg,0,256);
		memcpy(sms[counter].msg,msg.c_str(),msg.size());
		logging.println("sms","msg: ",msg);

		sms_remove(sms[counter].index);

		counter++;
	}while(new_message);

	//sms_remove_all();

	return;
}

bool Modem_BGxx::sms_send(string origin, string msg){

	string command = "AT+CMGS=\"" + origin + "\"";
	//if (!bgxx->checkAtCommand(command.c_str(), ">", 1000)) return false;
	bgxx->checkAtCommand(command.c_str(), ">", 3000);
	command = msg + "\x1A";
	if (!bgxx->checkAtCommand(command.c_str(), "OK", 7000)) return false;

	log(3,"sms sent");

	return true;
}


bool Modem_BGxx::sms_remove_all(){
	log(3,"removing all sms");

	string command = "AT+CMGD=1,4";

	if (!bgxx->checkAtCommand(command.c_str(), "OK", 1000)) return false;

	log(3,"all sms removed");

	return true;
}

bool Modem_BGxx::sms_remove(uint8_t index){
	logging.println("bgxx","removing sms: ",(int)index);

	string command = "AT+CMGD=" + to_string(index);

	if (!bgxx->checkAtCommand(command.c_str(), "OK", 1000)) return false;

	log(3,"sms removed");

	return true;
}


/*
// This function opens a socket and returns the respective socket id
bool Modem_BGxx::openSocket(uint8_t *socket, uint16_t port, uint16_t timeout) {

	HAL_Delay(1000);
	string rsp = "";
	// the socket is returned before OK
	#ifdef BC68

		//close(port);
		string s = "AT+NSOCR=DGRAM,17,"+to_string(port)+",1";
		rsp = bgxx->getAtCommandResponse(s.c_str(),timeout);
		if(rsp.size() > 0){
			rsp = rsp.substr(0,1);
			if(rsp[0] < 0x30 || rsp[0] > 0x39)
				return false;
      *socket = (uint8_t)stoi(rsp,nullptr,10);
			#ifdef (DEBUG_COMMANDS <= INFO_DEBUG_LEVEL)
				logging.println("modem","val: ",(int)*socket);
			#endif
			buffer_len[*socket]      = 0;
			connected_state[*socket] = true;
			//connected_until[*socket] = 0;
			return true;
		}
	#endif
	return false;
}
*/

/*
// This function opens an identified socket with the indicated port to receive data
bool Modem_BGxx::openSocket(uint8_t socket, uint16_t port, uint16_t timeout) {
	if (socket >= MAX_CONNECTIONS) return false;

	// the socket is returned before OK
	#ifdef SARAN2
		if (bgxx->checkAtCommand("AT+NSOCR=\"DGRAM\",17,"+string(port)+",1", "OK", timeout)) {
			buffer_len[socket]      = 0;
			connected_state[socket] = true;
			connected_until[socket] = 0;

			return true;
		}

	#endif

	return false;
}
*/

// --- SOCKET ---
bool Modem_BGxx::open(uint8_t context_id, uint8_t connect_id, string protocol, string host, uint16_t port) {

	string s = "AT+QIOPEN="+to_string(context_id)+","+to_string(connect_id)+",\""+protocol+"\",\""+host+"\","+to_string(port);
	string filter =  "+QIOPEN: "+to_string(connect_id)+",";
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),filter.c_str(), 6000);
	logging.println("BGxx","rsp: ",response);
	if(find(response,"0") > -1)
		return true;
	else return false;
}

// close socket
bool Modem_BGxx::close(uint8_t connect_id) {
	string s = "AT+QICLOSE="+to_string(connect_id);
	return bgxx->checkAtCommand(s.c_str(), "OK", 10000);
}

bool Modem_BGxx::send(uint8_t connect_id, string data, uint16_t length) {
	string s = "AT+QISEND="+to_string(connect_id)+","+to_string(length);
	if(bgxx->checkAtCommand(s.c_str(), ">", 3000)){
		if(bgxx->checkAtCommand(data.c_str(), "SEND OK", 6000)){
			logging.log("data sent","BGxx");
			return true;
		}else{
			logging.log("failed sending data","BGxx");
			return false;
		}
	}
	return false;
}

uint16_t Modem_BGxx::getData(uint8_t connect_id, uint16_t length){
	string s = "AT+QIRD="+to_string(connect_id)+","+to_string(length);
	bgxx->sendAtCommand(s.c_str());
	return bgxx->check_request(connect_id,length);
}

uint16_t Modem_BGxx::readData(uint8_t* data, uint16_t length){
	return bgxx->read_buffer(data,length);
}

string Modem_BGxx::getNewLine(uint8_t* data, uint16_t length){
	uint16_t len = bgxx->read_line(data,length);
	if(len > 0)
		return string((const char*)data);
	else
		return (string)"";
}

/* !! Not developed
	send data to an ip address
	Maximum data to sent is 512
	In the end checks if all data was sent to the respective port
bool Modem_BGxx::send_udp(uint16_t socket, string ipaddress, uint16_t port, char* data_c, uint16_t size) {

	if(size > 512){
		log(4,"size to large");
	}

	string data = "";
	uint8_t i = 0;

  char* hex = (char*)malloc(3);
  if(hex == nullptr)
    return false;

	while(i<size){
    memset(hex,0,3);
		sprintf(hex,"%x",data_c[i]);
		string a = string(hex);
		if(a.length() == 1)
			a = to_string(0) + a;
		data += a;

		i++;
	}

  free(hex);
	string s = "AT+NSOST="+to_string(cid.socket)+","+ipaddress+","+to_string(port)+","+to_string(size)+","+data;
	string response = bgxx->getAtCommandResponse(s.c_str(), 1000);

	//if(find(response,string(socket)+","+string(size)) != -1) return true;
	if(find(response,","+to_string(size)) != -1) return true;
	else return false;
}
*/

/* !! Not developed
	send data to an ip address
	Maximum data to sent is 512
	In the end checks if all data was sent to the respective port
bool Modem_BGxx::send_udp(uint16_t socket, string ipaddress, uint16_t port, char* data_c, uint16_t size, uint8_t tries) {

	if(size > 512){
		log(5,"size to large");
	}

	string data = "";
	uint8_t i = 0;

	char hex[3]; // why 3??

	string a = "";
	while(i<size){
		sprintf(hex,"%x",data_c[i]);
		a = string(hex);
		if(a.length() == 1)
			a = to_string(0) + a;
		data += a;

		i++;
	}

	i = 0;
	//string s = "AT+NSOST="+to_string(socket)+","+ipaddress+","+to_string(port)+","+to_string(size)+","+data;
	string s = "AT+NSOST="+to_string(cid.socket)+","+ipaddress+","+to_string(port)+","+to_string(size)+","+data;
	while(i < tries){
		string response = bgxx->getAtCommandResponse(s.c_str(), 3000);
		if(find(response,","+to_string(size)) != -1) return true;
		HAL_Delay(3000);
		i++;
	}

	change_state(MODEM_STATE_SETUP);
	//st.actual_state = MODEM_STATE_DISCONNECTED;
	return false;
}
*/

/*
// !! Not developed
// read data from a socket and stores it in a buffer -> buffers[socket]
// this function is called in an autonomous way
uint16_t Modem_BGxx::read_udp_socket(uint16_t socket, uint16_t size){
	if(size > 256)
		size = 256;

	string s = "AT+NSORF="+to_string(socket)+","+to_string(size);
	string response = bgxx->getAtCommandResponse(s.c_str(),1000);

	uint16_t length_data_read = 0, remote_port = 0;
	string data = "", ip = "";
	uint8_t socket_resp = 0, length_remaining_data = 0;

	if(find(response,",") != -1){ // response ok, read data
		if(! (find(response,",") > 0)) return false;
		socket_resp = stoi(response.substr(0,find(response,",")),nullptr,10);
		response = response.substr(find(response,",")+1);
		if(! (find(response,",") > 0)) return false;
		ip = response.substr(0,find(response,","));
		response = response.substr(find(response,",")+1);
		if(! (find(response,",") > 0)) return false;
		remote_port = stoi(response.substr(0,find(response,",")),nullptr,10); // port where messages were sent..
		response = response.substr(find(response,",")+1);
		if(! (find(response,",") > 0)) return false;
		length_data_read = stoi(response.substr(0,find(response,",")),nullptr,10); // port where messages were sent..toInt()
		response = response.substr(find(response,",")+1);
		if(! (find(response,",") > 0)) return false;
		data = response.substr(0,find(response,","));
		response = response.substr(response.find_last_of(",")+1);
		//if(! (find(response,"\n") > 0)) return false;
		length_remaining_data = stoi(response.substr(0,find(response,"\n")),nullptr,10);
	}else{
		log(5,"fail while reading udp socket");
		return false;
	}

	if( DEBUG_COMMANDS <= INFO_DEBUG_LEVEL ){
		sprintf(logging.log_msg,"-----New udp message-----");
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"socket: %d",socket_resp);
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"ip: %s",ip.c_str());
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"remote_port: %d",remote_port);
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"length_data_read: %d",length_data_read);
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"data: %s",data.c_str());
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"length_remaining_data: %d",length_remaining_data);
		logging.log(logging.log_msg,"bc68");
		sprintf(logging.log_msg,"-----End udp message-----");
		logging.log(logging.log_msg,"bc68");
	}
	// a flag should be launched

	char byte = 0;
	uint8_t a,b, i = 0, offset = 0;

	offset = buffer_len[socket];

	if( (offset+length_data_read) > BUFFER_SIZE){
		#ifdef DEBUG_MODEM_MESSAGE
			logging.log("buffer hasn't enough space","bc68");
		#endif
		return length_remaining_data;
	}

	while(i<(data.length()-1)){
		// atoi function was giving wrong values
		if(data[i] <= '9')
			a = data[i]-'0';
		else a = data[i]-'A'+10;

		if(data[i+1] <= '9')
			b = data[i+1]-'0';
		else b = data[i+1]-'A'+10;

		a = (a<<4)&0xF0;
		b = b&0x0F;
		byte = a | b ;

		buffers[socket][offset+i/2] = byte;
		i+=2;
	}

	buffer_len[socket] += length_data_read;
	//buffer_push(length_data_read); // store datagram length

	return length_remaining_data;

}
*/

// not needed
bool Modem_BGxx::close_pdp_context() {
	if (!bgxx->checkAtCommand("AT+QIDEACT=1", "OK", 3000)) return false;
	log(3,"PDP Context closed");
	return true;
}

// not needed
bool Modem_BGxx::disable_pdp() {
	if (!bgxx->checkAtCommand("AT+CGATT=0", "OK", 3000)) return false;
	log(3,"Dettached");
	return true;
}

bool Modem_BGxx::connected() {
	//print_state(state());
	//if(state() == MODEM_STATE_CONNECTED || state() == MODEM_STATE_PSM)
	if(state() == MODEM_STATE_CONNECTED)
		return true;

	//log(3,"Not connected");
	return false;

}

string Modem_BGxx::connected_radio(){ 		// get current radio in use
	return string(dev.radio);
}

bool Modem_BGxx::sleeping() {

	if(state() == MODEM_STATE_PSM)
		return true;

	return false;

}

void Modem_BGxx::check_signal(){

	string response = bgxx->getAtCommandResponse("AT+QCSQ","+QCSQ: ",2000);

	response.erase(std::remove(response.begin(), response.end(), '\"'), response.end());
	response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());
	response.erase(std::remove(response.begin(), response.end(), '\r'), response.end());
	int8_t index = find(response,",");
	if(index == -1 || response.length() == index) return;
	string aux = response.substr(0,index);
	memcpy(dev.radio,aux.c_str(),aux.length());
	aux = response.substr(index+1,response.length());
	if(isNumber(aux))
		dev.rssi = str2dec(aux);

}

int16_t Modem_BGxx::rssi() {

	return dev.rssi;
	/*
	string response = "";

	if(st.actual_state == MODEM_STATE_SHUTDOWN || st.actual_state == MODEM_STATE_PSM)
		return dev.rssi;

	response = bgxx->getAtCommandResponse("AT+CSQ","+CSQ:",1000);
	log(1,"response: "+response);
	if(find(response,",") > 0)
		response = (response.substr(0,find(response,","))); //.toInt();
	else return dev.rssi;

	int16_t value = 0;
	log(2,"rssi: " + response);

	if(response != "" && sizeof(response)>0)
		value = stoi(response,nullptr,10);

	dev.rssi = -113+(2*value);
	return dev.rssi;
	*/
}

// unjoin from apn -> cid

// choose what cid is on

string Modem_BGxx::getIP() {

	string s = "";

	if(state() == MODEM_STATE_SHUTDOWN || state() == MODEM_STATE_PSM){
		s = string(cid.ip);
		if(s.compare("") != 0)
			return string(cid.ip);
	}

	string response = bgxx->getAtCommandResponse("AT+CGPADDR=?", "+CGPADDR: (",200);
	log(3,response);

	if(response.size() == 0)
		return "";

	string v_cid = response.substr(0,1);

	s = "AT+CGPADDR="+v_cid;
	string f = "+CGPADDR: "+v_cid+",";
	response = bgxx->getAtCommandResponse(s.c_str(),f.c_str(),200);

	log(3,response);

	if (response.length() > 0) {
		uint8_t index1 = find(response,"OK");
		if(index1){
			string ip_address = response.substr(0,index1);
			log(3,"v_cid: "+v_cid+", ip address: " + ip_address);
			cid.id = stoi(v_cid,nullptr,10);
			memcpy(cid.ip,ip_address.c_str(),ip_address.size());
			return ip_address;
		}
	}
	return "";
}

string Modem_BGxx::getVersion(){

	bgxx->checkAtCommand("AT+QAPPVER", "OK", 2000);
	return bgxx->getAtCommandResponse("AT+CGMR", "OK", 2000);

}

string Modem_BGxx::getLocation(){

	bgxx->getAtCommandResponse("AT+QGPSLOC?", "OK", 400);

	return "";
}

// --- HTTP ---

bool Modem_BGxx::HTTP_get_fota(string host,uint16_t port,string address){

	string from = string(settings.fota.origin);
	//from.erase(std::remove(from.begin(), from.end(), '\"'), from.end());

	string s = "checking fota";
	sms_send(from,s);

	uint8_t socket = 2;
	string data = "GET " + address + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" +
                 "Cache-Control: no-cache\r\n" +
                 "Connection: close\r\n\r\n";

	close(socket);
	if(open(cid.id,socket,string("TCP"),host,port)){
		if(!send(socket,data,data.length())){
			log(3,"failure doing http request");
			s = "failure doing http request";
			sms_send(from,s);
			return false;
		}

		string filter = "+QIURC: \"recv\","+to_string(socket);
		bool has_data = true;

		uint8_t mux = 100;
		uint16_t len = 0, data_len = 0, header_len = 0;
		uint16_t frame = 0;
		string header;
		header.reserve(512);
		bool header_end = false;

		uint16_t arr_size = MODEMRXBUFFERSIZE;
		//uint16_t arr_size = 400;
		uint8_t* data = (uint8_t*)malloc(arr_size);
		uint8_t* aux_data = (uint8_t*)malloc(8);
		uint16_t written_bytes = 0, header_data = 0;
		bool end = false;

		uint32_t t = 0;
		if(data != nullptr){

			do{

				clear_WDT();

				t = millis();
				has_data = bgxx->getUnsolicitedCode(filter.c_str(),60*mux); // wait 6000ms to receive the first frame, then reduce it to 600ms
				mux = 1;

				len = getData(socket,arr_size-AT_OFFSET); // asks for 200 bytes

				if(len > 0){
					if(frame%10 == 0){
						logging.println("http","frame: ",(int)frame);
						logging.println("bgxx","total written bytes: ",(long)fw_flash.written_bytes());
					}

					if(header_end){
						len = readData(&data[header_data],len);
						uint16_t size = len + header_data;
						written_bytes = fw_flash.write_array(data,size);

						if(written_bytes == 0 && len+header_data >= 8){
							logging.log("error writing on memory, terminating fota","http");
							break;
						}

						if(len-written_bytes <= 8){
							// align data not written
							memcpy(aux_data,&data[written_bytes],len+header_data-written_bytes);
							memset(data,0,arr_size);
							header_data = len+header_data-written_bytes;
							memcpy(data,aux_data,header_data);
						}else logging.log("something went wrong","http");
					}

					data_len = 0;

					// header
					while(!header_end){
						memset(data,0,arr_size-AT_OFFSET);
						string line = getNewLine(data,arr_size-AT_OFFSET);

						data_len += line.length();

						header += line;
						header_len += (uint16_t)line.length();
						line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
						line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

						if(line.length() == 0){
							header_end = true;
							logging.log("header ended","bgxx");

							if(!parseHeader(header,header_len)){
								logging.log("couldn't start FOTA update","http");
								s = "couldn't start FOTA update";
								sms_send(from,s);

								free(aux_data);
								free(data);
								return false;
							}else{
								logging.println("http","body size: ",(long)content_length);
							  fw_flash.start_flash(content_length);
								fw_flash.expected_md5(md5);
								logging.log("FOTA ongoing","http");
							}

							// get body data in same frame of header
							if(data_len != len){ // it runs only once, after head parse

								logging.println("http","get remaining data: ",(int)len-data_len);

								len = readData(data,len-data_len);

								written_bytes = fw_flash.write_array(data,len);
								if(written_bytes == 0){
									logging.println("bgxx","file download terminated due to error writing on eeprom",written_bytes);

									free(aux_data);
									free(data);
									return false;
								}

								// align data not written
								memset(aux_data,0,8);
								memcpy(aux_data,&data[written_bytes],len-written_bytes);
								memset(data,0,arr_size);
								header_data = len-written_bytes;
								memcpy(data,aux_data,header_data);
							}

							break;
						}

						if(data_len == len || data_len > 1024)
							break;

					}

					frame++;

					if(header_end && fw_flash.check_size()) end = true;
				}

				if(len == 0){
					tries++;
					HAL_Delay(2000);
				}else tries = 0;

				if(tries > 3){
					logging.println("bgxx","missing_bytes bytes: ",(long)fw_flash.missing_bytes());

					logging.log("no more data available","http");
					logging.println("http","header_data: ",(int)header_data);

					if(fw_flash.missing_bytes() == 4){
						logging.log("write last word (uint32_t)","http");
						fw_flash.write_array_align((uint8_t*)data,(uint32_t)header_data);
					}

					end = true;
				}

			}while(!end);

		}
		free(aux_data);
		free(data);


		logging.println("bgxx","image size: ",(long)content_length);
		logging.println("bgxx","total written bytes: ",(long)fw_flash.written_bytes());

		if(fw_flash.check_size()){
			// validate md5
			logging.log("fw uploaded to flash, verifying md5","flash");
			unsigned char hash[16];
		  memset(hash,0,16);
			fw_flash.get_md5(hash,16);

			if(fw_flash.check_md5(hash)){

				s = "fw was uploaded successfully, proceeding to boot";
				sms_send(from,s);

				settings.fota.check = false;  // use it to clean bank after boot
				write_sequential_i2c(P_SETTINGS,PAGE_FOTA,(uint8_t*)settings.fota.host,sizeof(settings.fota));

				logging.log("booting from bank 2","flash");
				fw_flash.bootFromBank2();

				logging.log("error setting options to boot from bank 2","flash");
				s = "error setting options to boot from bank 2";
				sms_send(from,s);

				fw_flash.erase(FLASH_BANK_2);
			}else{
				logging.log("md5 verification failed","flash");
				s = "md5 verification failed";
				sms_send(from,s);

				fw_flash.erase(FLASH_BANK_2);
				settings.fota.check = false;  // use it to clean bank after boot
				write_sequential_i2c(P_SETTINGS,PAGE_FOTA,(uint8_t*)settings.fota.host,sizeof(settings.fota));
			}

		}else{
			logging.println("bgxx","total written bytes: ",(long)fw_flash.written_bytes());
			int32_t diff = fw_flash.missing_bytes();
			if(diff < 0)
				logging.println("bgxx","written more bytes thant it should: ",(long)abs(fw_flash.missing_bytes()));
			else
				logging.println("bgxx","missing bytes: ",(long)fw_flash.missing_bytes());


			logging.log("fw upload failed, wrong length","flash");
			s = "fw upload failed, wrong length";
			sms_send(from,s);
			return false;
		}

	}else{
		log(3,"couldn't establish tcp connection");
		s = "couldn't establish tcp connection";
		sms_send(from,s);
		return false;
	}

	return false;
}

uint16_t Modem_BGxx::HTTP_get_config(uint8_t* body, string host,string address){

	uint8_t socket = 2;
	string data = "GET " + address + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" +
                 "Cache-Control: no-cache\r\n" +
                 "Connection: close\r\n\r\n";

	close(socket);
	if(open(cid.id,socket,string("TCP"),host,80)){
		if(!send(socket,data,data.length())){
			log(3,"failure doing http request");
			return 0;
		}

		string filter = "+QIURC: \"recv\","+to_string(socket);
		bool has_data = true;

		uint8_t mux = 100;
		uint16_t len = 0, data_len = 0, header_len = 0;
		string header;
		header.reserve(512);
		bool header_end = false;

		uint16_t arr_size = MODEMRXBUFFERSIZE;
		uint8_t data[MODEMRXBUFFERSIZE];// = (uint8_t*)malloc(MODEMRXBUFFERSIZE);
		uint16_t header_data = 0;
		bool end = false;
		uint32_t t = 0, body_size = 0;

		uint32_t timeout = millis()+15000;
		if(true){// data != nullptr){
			do{

				clear_WDT();

				has_data = bgxx->getUnsolicitedCode(filter.c_str(),60*mux); // wait 6000ms to receive the first frame, then reduce it to 600ms
				if(has_data)
					logging.log("bg95","has data");
				mux = 1;

				len = arr_size-AT_OFFSET;
				if(header_end && body_size+(arr_size-AT_OFFSET) > content_length){
					len = content_length - body_size;
				}

				len = getData(socket,len); // asks for len bytes

				if(len > 0){ // data was read

					if(header_end){ // read body and store on data variable
						len = readData(&body[body_size],len);
						body_size += len;
					}

					data_len = 0;

					// header
					while(!header_end){ // reading data from header
						memset(data,0,MODEMRXBUFFERSIZE);
						string line = getNewLine(data,arr_size-AT_OFFSET);
						if(line.length() == 0)
							break;
						if( DEBUG_COMMANDS <= VERBOSE_DEBUG_LEVEL)
							logging.println("http","line: ",line);
						data_len += line.length();

						header += line;
						header_len += (uint16_t)line.length();
						line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
						line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

						if(line.length() == 0){
							header_end = true;

							if(!parseHeader(header,header_len)){
								if( DEBUG_COMMANDS <= WARNING_DEBUG_LEVEL)
									logging.log("failure parsing header","http");
								return 0;
							}else{
								if( DEBUG_COMMANDS <= VERBOSE_DEBUG_LEVEL)
									logging.println("http","body size: ",(long)content_length);
							}

							// get body data in same frame of header
							if(data_len != len){ // it runs only once, after head parse
								len = readData(&body[0],len-data_len);
								body_size = len;
								//logging.log_arr(body,body_size);
							}

							break;
						}

						if(data_len == len || data_len == MODEMRXBUFFERSIZE)
							break;
					}
				}

				if(len == 0){
					tries++;
					HAL_Delay(2000);
				}else tries = 0;

				if(tries > 3){
					logging.log("no more data available","http");
					end = true;
				}else if(content_length != 0 && body_size == content_length){
					if( DEBUG_COMMANDS <= VERBOSE_DEBUG_LEVEL)
						logging.log("done","http");
					end = true;
				}
				//logging.println("http","timeout: ",(long)(timeout-millis()));
				//logging.println("http","end: ",(int)end);
			}while(!end && timeout > millis());
		}else return 0;

		uint8_t hash[16];
		uint8_t md5_expected[16];

		MD5Init(&mdctx);
		MD5Update(&mdctx,body,body_size);
		MD5Final(hash,&mdctx);

		uint8_t i = 0;
		while(i*2<md5.length()){
			md5_expected[i] = str2hex(md5.substr(i*2,2));
			i++;
		}

		if( DEBUG_COMMANDS <= VERBOSE_DEBUG_LEVEL){
			logging.log_hex("header md5: ",md5_expected,16);
			logging.log_hex("calc hash md5: ",hash,16);
		}

		i = 0;
		while(i<16){
			if(hash[i] != md5_expected[i]){
				log(3,"invalid checksum");
				return 0;
			}
			i++;
		}

		memset(settings.fw.hash,0,sizeof(settings.fw.hash));
		memcpy(settings.fw.hash,md5.c_str(),md5.length());
		write_sequential_i2c(P_SETTINGS,PAGE_FW,(uint8_t*)&settings.fw.model,sizeof(settings.fw));
		return body_size;
	}else{
		log(3,"couldn't establish tcp connection");
		return 0;
	}

	return 0;
}

bool Modem_BGxx::parseHeader(string header,uint16_t header_len){
	logging.println("bgxx","header: \n",header);

	bool http_ok = false;
	int16_t index = 0;
	uint16_t len = 0, frame = 0;
	string ctl_filter = "Content-Length: ";
	string md5_filter = "Content-MD5: ";

	// header_len = 20726 ?? how is this possible?
	uint32_t timeout = millis() + 5000;
	while(len < header_len && timeout > millis()){
		index = find(header,"\n");

		if(index > -1){
			len += index+1;

			string line = header.substr(0,index-1);
			header = header.substr(index+1);

			if(frame == 0){
				if(find(line,"200 OK") == -1){
					logging.log("http error","http");
					return false;
				}else
					http_ok = true;
			}

			// body length
			index = find(line,ctl_filter);
			if(index > -1){
				string ctl = line.substr(index+ctl_filter.length());
				if(has_only_digits(ctl)){
					content_length = std::stoul(ctl);
				}
			}

			// md5 hash
			index = find(line,md5_filter);
			if(index > -1){
				md5 = line.substr(index+md5_filter.length());
			}

			frame++;
		}
	}

	if(http_ok && content_length > 0) return true;
	else return false;
}

bool Modem_BGxx::update(string host,uint16_t port,string address){
  string s = "AT+QFOTADL=\""+host+address+"\"";
 	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),"+QIND: \"FOTA\",\"HTTPSTART\"",5000);

  if(response.length() == 0){
    logging.log("Fota request has failed","BC660");
    return false;
  }

  uint32_t timeout = millis()+3600000;

  while(timeout > millis()){
    clear_WDT();
    if(bgxx->getUnsolicitedCode("RDY",5000)){
      logging.log("fota has finished","bc660");
      reset();
      return true;
    }
  }
  logging.log("fota timeout","bc660");
  return false;
}

// --- MQTT ---

void Modem_BGxx::MQTT_init(void(*callback)(string)) {
	parseMQTTmessage = callback;
}

bool Modem_BGxx::MQTT_setup(uint8_t client,string will_topic) {
	if(client > 5)
		return client;

	mqtt_client = client;
	string s = "AT+QMTCFG=\"pdpcid\","+to_string(client)+","+to_string(cid.id);
	if(!bgxx->checkAtCommand(s.c_str(),"OK",1000))
		return false;

	s = "AT+QMTCFG=\"recv/mode\","+to_string(client)+",1,0";
	if(!bgxx->checkAtCommand(s.c_str(),"OK",1000))
		return false;

	/*
	s = "AT+QMTCFG=\"will\","+to_string(client)+",1,2,1,\""+will_topic+"\",\"offline\"";
	return bgxx->checkAtCommand(s.c_str(),"OK",1000);
	*/
	connected_state[client] = 0;
}

bool Modem_BGxx::MQTT_open(uint8_t client) {

	if(client > 5)
		return client;

	if(MQTT_isOpened(client))
		return true;

	mqtt_tries++;
	logging.println("mqtt","tries: ",mqtt_tries);
	if(mqtt_tries == 15){
		mqtt_tries = 0;
		reset();
	}

	string s = "AT+QMTOPEN="+to_string(client)+",\""+string(settings.mqtt.host)+"\","+to_string(settings.mqtt.port);
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),"+QMTOPEN: 1,",10000);
	if(response.length() > 0){
		if(isdigit(response.c_str()[0])){
			int8_t res = stoi(response.substr(0,1),nullptr,10);
			connected_state[client] = res;
		}
	}
	/*
	if(connected_state[client] == 2)
		MQTT_close(client);
	*/

	return MQTT_isOpened(client);
}

bool Modem_BGxx::MQTT_isOpened(uint8_t client) {

	string s = "AT+QMTOPEN?";//+to_string(client)+",\""+string(settings.mqtt.host)+"\","+to_string(settings.mqtt.port);
	string ans = "+QMTOPEN: "+to_string(client)+",\""+string(settings.mqtt.host)+"\","+to_string(settings.mqtt.port);
	return bgxx->checkAtCommand(s.c_str(),ans.c_str(),2000);
}

int8_t Modem_BGxx::MQTT_close(uint8_t client) {
	if(client > MAX_CONNECTIONS)
		return client;

	string s = "AT+QMTCLOSE="+to_string(client);
	string f = "+QMTCLOSE: "+to_string(client)+",";
	string response = bgxx->getAtCommandResponse(s.c_str(),f.c_str(),10000);

	if(response.length() > 0){
		if(isdigit(response.c_str()[0]))
		connected_state[client] = 0;
		return (int)stoi(response);
	}else return 0;
}

int8_t Modem_BGxx::MQTT_connect(uint8_t client) {
	if(client > MAX_CONNECTIONS)
		return client;

	MQTT_checkConnection(client);

	if(!connected_state[client]){
		if(!MQTT_open(client))
			return -1;
	}

	string uid = getImei();
	if(uid == "")
		return -1;

	logging.println("bgxx","uid:",uid.c_str());
	string s = "AT+QMTCONN="+to_string(client)+",\""+uid+"\",\""+string(settings.mqtt.user)+"\",\""+string(settings.mqtt.pass)+"\"";

	string ans = "QMTCONN: "+to_string(client)+",0,0";
	if(bgxx->checkAtCommand(s.c_str(),ans.c_str(),10000)){
		mqtt_connected = true;
		return 0;
	}else{
		mqtt_connected = false;
		return -1;
	}
	/*
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),"+QMTCONN: ",3000);
	logging.println("bgxx","res:",response);
	if(find(response,to_string(client)+",0,0") > -1)
		return 0;
	return -1;
	*/
}

bool Modem_BGxx::MQTT_connected(uint8_t client){
	return mqtt_connected;
}

/*
	1 MQTT is initializing
	2 MQTT is connecting
	3 MQTT is connected
	4 MQTT is disconnecting
*/
int8_t Modem_BGxx::MQTT_checkConnection(uint8_t client){
	if(client > 5)
		return client;

	string s = "AT+QMTCONN?";
	string f = "+QMTCONN: "+to_string(client)+",";
	string response = bgxx->getAtCommandResponse(s.c_str(),f.c_str(),2000);

	if(response.length() > 0){
		int8_t rsp = 0;
		if(isdigit(response.c_str()[0]))
			rsp = (int)stoi(response);
		if(rsp == 3)
			mqtt_connected = true;
		else mqtt_connected = false;
		return rsp;
	}else{
		mqtt_connected = false;
		return 0;
	}
}

/*
	-1 Failed to close connection
	0 Connection closed successfully
*/
int8_t Modem_BGxx::MQTT_disconnect(uint8_t client) {
	if(client > 5)
		return client;

	string s = "AT+QMTDISC="+to_string(client);
	string f = "+QMTDISC: "+to_string(client)+",";
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),f.c_str(),10000);

	if(response.length() > 0){
		int8_t rsp = -1;
		if(isdigit(response.c_str()[0]))
			rsp = (int)stoi(response);
		if(rsp == 0)
			mqtt_connected = false;
		return rsp;
	}else return 0;
}

bool Modem_BGxx::MQTT_subscribeTopic(uint8_t client, uint16_t msg_id, string topic,uint8_t qos, uint8_t len) {
	if(client > 5)
		return client;

	std::string s;
	s.reserve(512);
	s = "AT+QMTSUB="+to_string(client)+","+to_string(msg_id);
	s += ",\""+topic+"\","+to_string(qos);


	string f = "+QMTSUB: "+to_string(client)+","+to_string(msg_id)+",";
	//logging.println("BGxx","string: ",s);
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),f.c_str(),3000);
	int8_t index = find(response,",");
	logging.println("BGxx","response: ",response);
	if(index > -1){
		response = response.substr(0,index);
		if(has_only_digits(response)){
			if((int8_t)stoi(response) == 0){
				log(3,"packet sent successfully");
				return true;
			}
		}
	}
	return false;
}


/*
0 Sent packet successfully and received ACK from server
1 Packet retransmission
2 Failed to send packet
*/
bool Modem_BGxx::MQTT_subscribeTopics(uint8_t client, uint16_t msg_id, string topic[],uint8_t qos[], uint8_t len) {
	if(client > 5)
		return client;

	std::string s;
	s.reserve(512);
	s = "AT+QMTSUB="+to_string(client)+","+to_string(msg_id);
	uint8_t i = 0;
	while(i<len){
		s += ",\""+topic[i]+"\","+to_string(qos[i]);
		i++;
	}

	string f = "+QMTSUB: "+to_string(client)+","+to_string(msg_id)+",";
	//logging.println("BGxx","string: ",s);
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),f.c_str(),18000);
	int8_t index = find(response,",");
	logging.println("BGxx","response: ",response);
	if(index > -1){
		response = response.substr(0,index);
		if(has_only_digits(response)){
			if((int8_t)stoi(response) == 0){
				log(3,"packet sent successfully");
				return true;
			}
		}
	}
	return false;
}

/*
0 Packet sent successfully and ACK received from server (message that
published when <qos>=0 does not require ACK)
1 Packet retransmission
2 Failed to send packet
*/
int8_t Modem_BGxx::MQTT_unSubscribeTopic(uint8_t client, uint16_t msg_id, string topic[], uint8_t len) {
	if(client > 5)
		return client;

	string s = "AT+QMTUNS="+to_string(client)+","+to_string(msg_id);
	uint8_t i = 0;
	while(i<len){
		s += ","+topic[i];
		i++;
	}

	string f = "+QMTUNS: "+to_string(client)+","+to_string(msg_id)+",";
	string response = bgxx->getAtCommandResponse(s.c_str(),f.c_str(),10000);
	response = response.substr(0,1);
	return (int8_t)stoi(response);
}

/*
	return
	-1 error
	0 Packet sent successfully and ACK received from server (message that
	published when <qos>=0 does not require ACK)
	1 Packet retransmission
	2 Failed to send packet
*/
int8_t Modem_BGxx::MQTT_publish(uint8_t client, uint16_t msg_id,uint8_t qos, uint8_t retain, string topic, string msg, bool ts) {
	if(client > 5)
		return client;

	if(!mqtt_connected) return -1;

	string payload = "";
	if(ts)
	 	payload = "{\"ts\":"+to_string(getTimestamp())+",\"val\":\""+msg+"\"}";
	else
		payload = msg;

	string s = "AT+QMTPUBEX="+to_string(client)+","+to_string(msg_id)+","+to_string(qos)+","+to_string(retain)+",\""+topic+"\",\""+payload+"\"";
	string f = "+QMTPUB: "+to_string(client)+","+to_string(msg_id)+",";
	string response = bgxx->getAtCommandResponseNoOK(s.c_str(),f.c_str(),10000);
	response = response.substr(0,1);

	if(response.length() > 0){
		if(isdigit(response.c_str()[0])){
			return (int)stoi(response);
		}else{
			if( DEBUG_MQTT <= ERROR_DEBUG_LEVEL)
				logging.log("error sending message","mqtt");
		}
	}
	return -1;
}

string Modem_BGxx::MQTT_readMessages(uint8_t client) {
	if(client > 5)
		return "";

	string s = "";
	int8_t i = 0;
	bool read = false;

	/* if at least 1 buffer has data, it will check the others also */
	while(i<5){
		if(mqtt_buffer[i++] != -1)
			read = true;
	}

	i = 0;
	if(read){
		while(i<5){
			s = "AT+QMTRECV="+to_string(client)+","+to_string(i);
			bgxx->getAtCommandResponse(s.c_str(),400);
			mqtt_buffer[i++] = -1;
		}
	}

	return "";
}

// --- private ---

// log functions

const char* Modem_BGxx::print_event(uint8_t event){
	#ifdef DEBUG_MODEM_INFO
	if(event == MODEM_EVENT_UNKNOW)
		return "MODEM_EVENT_UNKNOW";
	else if(event == MODEM_EVENT_CONNECT)
		return "MODEM_EVENT_CONNECT";
	else if(event == MODEM_EVENT_DISCONNECT)
		return "MODEM_EVENT_DISCONNECT";
	else if(event == MODEM_EVENT_SWITCH_OFF_RADIO)
		return "MODEM_EVENT_SWITCH_OFF_RADIO";
	else return "NO ENVENT";
	#endif
}

const char* Modem_BGxx::print_state(uint8_t state){
	#ifdef DEBUG_MODEM_INFO
	if(state == MODEM_STATE_UNKNOW)
		return "MODEM_STATE_UNKNOW";
	else if(state == MODEM_STATE_CONNECTED)
		return "MODEM_STATE_CONNECTED";
	else if(state == MODEM_STATE_DISCONNECTED)
		return "MODEM_STATE_DISCONNECTED";
	else if(state == MODEM_STATE_SAVEMODE)
		return "MODEM_STATE_SAVEMODE";
	else if(state == MODEM_STATE_CONNECTING)
		return "MODEM_STATE_CONNECTING";
	else if(state == MODEM_STATE_PSM)
		return "MODEM_STATE_PSM";
	else if(state == MODEM_STATE_SHUTDOWN)
		return "MODEM_STATE_SHUTDOWN";
	else return "UNKNOWN";
	#endif

}


void Modem_BGxx::log_info() {
	#ifdef DEBUG_MODEM_INFO
	log(2,"--- bgxx state machine ---");
  char* text = (char*)malloc(256);
  if(text != nullptr){
      sprintf(text,"actual_state: %s",print_state(st.actual_state));
      log(2,text);
      sprintf(text,"previous_state: %s",print_state(st.previous_state));
      log(2,text);
      sprintf(text,"event: %s",print_event(st.event));
      log(2,text);
      sprintf(text,"timeout: %lu",(long)st.timeout);
      log(2,text);
      sprintf(text,"tries: %d",(int)st.tries);
      log(2,text);

      free(text);
  }
	log(2,"--- --- --- --- --- ---");
	#endif
}

void Modem_BGxx::log(uint8_t level, string text) {

	if(level >= DEBUG_COMMANDS ){
		if(level == ERROR_DEBUG_LEVEL)
			logging.log_nnl("!! error","bgxx");
		if(level == ERROR_DEBUG_LEVEL)
			logging.log_nnl("!! warning","bgxx");
		logging.log((char*)text.c_str(),"bgxx");
	}

}

void Modem_BGxx::log(uint8_t level, const char* text) {
	if(level >= DEBUG_COMMANDS ){
		if(level == ERROR_DEBUG_LEVEL)
			logging.log_nnl("!! error","bgxx");
		if(level == ERROR_DEBUG_LEVEL)
			logging.log_nnl("!! warning","bgxx");
		logging.log((char*)text,"bgxx");
	}
}
/*
bool Modem_BGxx::buffer_push(uint8_t byte){

	if(!free_space())
		return false;

	q[tail] = byte;
	if(tail > 0)
		tail--;
	else
		tail = TASKS_MAX-1;

	return true;
}

uint16_t Modem_BGxx::buffer_empty(){
	if(head > tail)
		return q.size() - (head-tail);
	else if(head == tail)
		return q.size();
	else if(head < tail)
		return head-tail;
}

uint8_t Modem_BGxx::buffer_front(){

bool Modem_BGxx::buffer_pop(){

	clean_task();

	if(head > 0)
		head--;
	else
		head = TASKS_MAX-1;

	return true;
}
*/
