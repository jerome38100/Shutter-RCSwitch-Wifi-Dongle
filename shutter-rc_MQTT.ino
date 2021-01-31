/*  Controlling Jarolift TDEF 433MHZ radio shutters and SOMFY shuuter via ESP8266 and CC1101 Transceiver Module in asynchronous mode.
    Including RCSwitch library to receive or send 433 RF signal
    Copyright (C) 2017-2018 Steffen Hille et al.
    Copyright (C) 2020-2021 Jerome Dalle.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Softwasere Foundation, either version 3 of the License, or
    (at yours option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Changelog: see CHANGES.md
/*
  Kanal  S/N           DiscGroup_8-16             DiscGroup_1-8     SN(last two digits)
  0       0            0000 0000                   0000 0001           0000 0000
  1       1            0000 0000                   0000 0010           0000 0001
  2       2            0000 0000                   0000 0100           0000 0010
  3       3            0000 0000                   0000 1000           0000 0011
  4       4            0000 0000                   0001 0000           0000 0100
  5       5            0000 0000                   0010 0000           0000 0101
  6       6            0000 0000                   0100 0000           0000 0110
  7       7            0000 0000                   1000 0000           0000 0111
  8       8            0000 0001                   0000 0000           0000 0111
  9       9            0000 0010                   0000 0000           0000 0111
  10      10           0000 0100                   0000 0000           0000 0111
  11      11           0000 1000                   0000 0000           0000 0111
  12      12           0001 0000                   0000 0000           0000 0111
  13      13           0010 0000                   0000 0000           0000 0111
  14      14           0100 0000                   0000 0000           0000 0111
  15      15           1000 0000                   0000 0000           0000 0111
*/

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <FS.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <DoubleResetDetector.h>
#include <simpleDSTadjust.h>
#include <coredecls.h>              // settimeofday_cb()
#include <string>
#include <sstream>
#include <exception>

#include "helpers.h"
#include "global.h"
#include "html_api.h"

extern "C" {
#include "user_interface.h"
#include "Arduino.h"
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include <KeeloqLib.h>
#include "RCSwitch.h"
}

// Number of seconds after reset during which a
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

// User configuration
#define Lowpulse         400    // Defines pulse-width in microseconds. Adapt for your use...
#define Highpulse        800
#define SYMBOL 604

#define BITS_SIZE          8
byte syncWord            = 199;
int device_key_msb       = 0x0; // stores cryptkey MSB
int device_key_lsb       = 0x0; // stores cryptkey LSB
uint64_t button          = 0x0; // 1000=0x8 up, 0100=0x4 stop, 0010=0x2 down, 0001=0x1 learning
int disc                 = 0x0;
uint32_t dec             = 0;   // stores the 32Bit encrypted code
uint64_t pack            = 0;   // Contains data to send.
byte disc_low[16]        = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0,  0x0,  0x0,  0x0};
byte disc_high[16]       = {0x0, 0x0, 0x0, 0x0, 0x0,  0x0,  0x0,  0x0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80};
byte disc_l              = 0;
byte disc_h              = 0;
int adresses[]          = {5, 11, 17, 23, 29, 35, 41, 47, 53, 59, 65, 71, 77, 85, 91, 97,2254,2260,2266,2272,2278,2284,2290,2296,2302,2308,2314,2320,2326,2332,2338,2442}; // Defines start addresses of channel data stored in EEPROM 4bytes s/n.
uint64_t new_serial      = 0;
byte marcState;
int MqttRetryCounter = 0;                 // Counter for MQTT reconnect

// RX variables and defines
#define debounce         200              // Ignoring short pulses in reception... no clue if required and if it makes sense ;)
#define pufsize          216              // Pulsepuffer
#define TX_PORT            4              // Outputport for transmission
#define RX_PORT            5              // Inputport for reception
uint32_t rx_serial       = 0;
char rx_serial_array[8]  = {0};
char rx_disc_low[8]      = {0};
char rx_disc_high[8]     = {0};
uint32_t rx_hopcode      = 0;
uint16_t rx_disc_h       = 0;
byte rx_function         = 0;
int rx_device_key_msb    = 0x0;           // stores cryptkey MSB
int rx_device_key_lsb    = 0x0;           // stores cryptkey LSB
volatile uint32_t decoded         = 0x0;  // decoded hop code
volatile byte pbwrite;
volatile unsigned int lowbuf[pufsize];    // ring buffer storing LOW pulse lengths
volatile unsigned int hibuf[pufsize];     // ring buffer storing HIGH pulse lengths
volatile bool iset = false;
volatile byte value = 0;                  // Stores RSSI Value
long rx_time;
int steadycnt = 0;
boolean time_is_set_first = true;
DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);
uint8_t PA_JAROLIFT[8]  {0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t PA_SOMFY[8]     {0x00,0xC0,0x00,0x00,0x00,0x00,0x00,0x00};
                         // The connection to the hardware chip CC1101 the RF Chip
RCSwitch mySwitch = RCSwitch();

// forward declarations
void ICACHE_RAM_ATTR radio_rx_measure();

//####################################################################
// sketch initialization routine
//####################################################################
void setup()
{
  InitLog();
  EEPROM.begin(4096);
  Serial.begin(115200);
  settimeofday_cb(time_is_set);
  updateNTP(); // Init the NTP time
  WriteLog("[INFO] - starting Shutter-RC Dongle " + (String)PROGRAM_VERSION, true);
  WriteLog("[INFO] - ESP-ID " + (String)ESP.getChipId() + " // ESP-Core  " + ESP.getCoreVersion() + " // SDK Version " + ESP.getSdkVersion(), true);
    
  // callback functions for WiFi connect and disconnect
  // placed as early as possible in the setup() function to get the connect
  // message catched when the WiFi connect is really fast
  gotIpEventHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP & event)
  {
    WriteLog("[INFO] - WiFi station connected - IP: " + WiFi.localIP().toString(), true);
    wifi_disconnect_log = true;
  });

  disconnectedEventHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected & event)
  {
    if (wifi_disconnect_log) {
      WriteLog("[INFO] - WiFi station disconnected", true);
      // turn off logging disconnect events after first occurrence, otherwise the log is filled up
      wifi_disconnect_log = false;
    }
  });

  InitializeConfigData();
  EEPROM.get(jaroliftadr, jaroliftcnt);
  EEPROM.get(somfyadr, somfycnt);

  // initialize the transceiver chip
  WriteLog("[INFO] - initializing the CC1101 Transceiver. If you get stuck here, it is probably not connected.", true);

 //CC1101 Settings:               
  ELECHOUSE_cc1101.Init();        // must be set to initialize the cc1101!
  ELECHOUSE_cc1101.setMHZ(433.92);   
  ELECHOUSE_cc1101.SetRx();
  pinMode(led_pin, OUTPUT);   // prepare LED on ESP-Chip

  // test if the WLAN SSID is on default
  // or DoubleReset detected
  if ((drd.detectDoubleReset()) || (config.ssid == "MYSSID")) {
    digitalWrite(led_pin, LOW);  // turn LED on                    // if yes then turn on LED
    AdminEnabled = true;                                           // and go to Admin-Mode
  } else {
    digitalWrite(led_pin, HIGH); // turn LED off                   // turn LED off
  }

  // enable access point mode if Admin-Mode is enabled
  if (AdminEnabled)
  {
    WriteLog("[WARN] - Admin-Mode enabled!", true);
    WriteLog("[WARN] - starting soft-AP ... ", false);
    wifi_disconnect_log = false;
    WiFi.mode(WIFI_AP);
    WriteLog(WiFi.softAP(ACCESS_POINT_NAME, ACCESS_POINT_PASSWORD) ? "Ready" : "Failed!", true);
    WriteLog("[WARN] - Access Point <" + (String)ACCESS_POINT_NAME + "> activated. WPA password is " + ACCESS_POINT_PASSWORD, true);
    WriteLog("[WARN] - you have " + (String)AdminTimeOut + " seconds time to connect and configure!", true);
    WriteLog("[WARN] - configuration webserver is http://" + WiFi.softAPIP().toString(), true);
  }
  else
  {
    // establish Wifi connection in station mode
    ConfigureWifi();
  }

  // configure webserver and start it
  server.on ( "/api", html_api );                       // command api
  SPIFFS.begin();                                       // Start the SPI flash filesystem
  server.onNotFound([]() {                              // If the client requests any URI
    if (!handleFileRead(server.uri())) {                // send it if it exists
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
      Serial.println(" File not found: did you upload the data directory?");
    }
  });

  server.begin();
  WriteLog("[INFO] - HTTP server started", true);
  tkHeartBeat.attach(1, HeartBeat);

  // configure MQTT client
  mqtt_client.setServer(IPAddress(config.mqtt_broker_addr[0], config.mqtt_broker_addr[1],
                                  config.mqtt_broker_addr[2], config.mqtt_broker_addr[3]),
                        config.mqtt_broker_port.toInt());
  mqtt_client.setCallback(mqtt_callback);   // define Handler for incoming messages
  mqttLastConnectAttempt = 0;

  pinMode(TX_PORT, OUTPUT);       // TX Pin
  pinMode(RX_PORT, INPUT_PULLUP); // RX Pin
 
} // void setup

//####################################################################
// main loop
//####################################################################
void loop()
{
  // Call the double reset detector loop method every so often,
  // so that it can recognise when the timeout expires.
  // You can also call drd.stop() when you wish to no longer
  // consider the next reset as a double reset.
  drd.loop();

  // disable Admin-Mode after AdminTimeOut
  if (AdminEnabled)
  {
    if (AdminTimeOutCounter > AdminTimeOut / HEART_BEAT_CYCLE)
    {
      AdminEnabled = false;
      digitalWrite(led_pin, HIGH);   // turn LED off
      WriteLog("[WARN] - Admin-Mode disabled, soft-AP terminate ...", false);
      WriteLog(WiFi.softAPdisconnect(true) ? "success" : "fail!", true);
      ConfigureWifi();
    }
  }
  server.handleClient();

// To enable RCSwitch receive and decode protocol function 
  mySwitch.enableReceive(RX_PORT);
  mySwitch.enableTransmit(TX_PORT);

   if(mySwitch.available())
    {
     // send mqtt message with received RF 433 Data:
     if (mqtt_client.connected() && mqtt_send_radio_receive_all) {
      String Topic = "tele/" + config.mqtt_devicetopic + "/rfreceived";
      const char * msg = Topic.c_str();
      char payload[220];
      snprintf(payload, sizeof(payload),
       "{\"code\":\"0x%08x\", \"bitlength\":\"0x%x\", \"protocol\":\"0x%02x\"}",
       mySwitch.getReceivedValue(),mySwitch.getReceivedBitlength(),mySwitch.getReceivedProtocol() );
      mqtt_client.publish(msg, payload);
      WriteLog("[INFO] - received remote: " + String(payload), true);
     }
     mySwitch.resetAvailable();
    }
// End receive RcSwitch function

/* To receive Jarolift remote command code
  attachInterrupt(RX_PORT, radio_rx_measure, CHANGE); // Interrupt on change of RX_PORT

 if (iset) {
  cc1101.cmdStrobe(CC1101_SCAL);
  delay(50);
  EnterRx();
  iset = false;
  delay(200);
  attachInterrupt(RX_PORT, radio_rx_measure, CHANGE); // Interrupt on change of RX_PORT
 }
 
  // Check if RX buffer is full
 if ((lowbuf[0] > 3650) && (lowbuf[0] < 4300) && (pbwrite >= 65) && (pbwrite <= 75)) {     // Decode received data...
   if (debug_log_radio_receive_all)
     WriteLog("[INFO] - received data", true);
   iset = true;
   ReadRSSI();
   pbwrite = 0;

   for (int i = 0; i <= 19; i++) {     // extracting Hopcode
     if (lowbuf[i + 1] < hibuf[i + 1]) {
       rx_hopcode = rx_hopcode & ~(1 << i) | (0 << i);
     } else {
       rx_hopcode = rx_hopcode & ~(1 << i) | (1 << i);
     }
   }

   for (int i = 0; i <= 15; i++) {     // extracting Hopcode
     if (lowbuf[i + 1] < hibuf[i + 1]) {
       rx_hopcode = rx_hopcode & ~(1 << i) | (0 << i);
     } else {
       rx_hopcode = rx_hopcode & ~(1 << i) | (1 << i);
     }
   }
   
   for (int i = 0; i <= 27; i++) {    // extracting Serialnumber  
    if (lowbuf[i + 33] < hibuf[i + 33]) {
       rx_serial = rx_serial & ~(1 << i) | (0 << i);
     } else {
       rx_serial = rx_serial & ~(1 << i) | (1 << i);
     }
   }
   rx_serial_array[0] = (rx_serial >> 24) & 0xFF;
   rx_serial_array[1] = (rx_serial >> 16) & 0xFF;
   rx_serial_array[2] = (rx_serial >> 8) & 0xFF;
   rx_serial_array[3] = rx_serial & 0xFF;

   for (int i = 0; i <= 3; i++) {                        // extracting function code
     if (lowbuf[61 + i] < hibuf[61 + i]) {
       rx_function = rx_function & ~(1 << i) | (0 << i);
     } else {
       rx_function = rx_function & ~(1 << i) | (1 << i);
     }
   }

   for (int i = 0; i <= 7; i++) {                        // extracting high disc
     if (lowbuf[65 + i] < hibuf[65 + i]) {
       rx_disc_h = rx_disc_h & ~(1 << i) | (0 << i);
     } else {
       rx_disc_h = rx_disc_h & ~(1 << i) | (1 << i);
     }
   }

   rx_disc_high[0] = rx_disc_h & 0xFF;
   rx_keygen ();
   rx_decoder();
   if (rx_function == 0x4)steadycnt++;           // to detect a long press....
   else steadycnt--;
   if (steadycnt > 10 && steadycnt <= 40) {
     rx_function = 0x3;
     steadycnt = 0;
   }

   Serial.printf(" serialnumber: 0x%08x // function code: 0x%02x // disc: 0x%02x\n\n", rx_serial, rx_function, rx_disc_h);

   // send mqtt message with received Data:
   if (mqtt_client.connected() && mqtt_send_radio_receive_all) {
     String Topic = "stat/" + config.mqtt_devicetopic + "/received";
     const char * msg = Topic.c_str();
     char payload[220];
      snprintf(payload, sizeof(payload),
              "{\"serial\":\"0x%08x\", \"rx_function\":\"0x%x\", \"rx_disc_low\":%d, \"rx_disc_high\":%d, \"RSSI\":%d, \"counter\":%d, \"rx_device_key_lsb\":\"0x%08x\", \"rx_device_key_msb\":\"0x%08x\", \"decoded\":\"0x%08x\"}",
              rx_serial, rx_function, rx_disc_low[0], rx_disc_h, value, rx_disc_low[3], rx_device_key_lsb, rx_device_key_msb, decoded );
     mqtt_client.publish(msg, payload);
    //WriteLog("[INFO] - received jarolift remote: " + String(payload), true);
  }

   rx_disc_h = 0;
   rx_hopcode = 0;
   rx_function = 0;
 }
*/

  // If you do not use a MQTT broker so configure the address 0.0.0.0
  if (config.mqtt_broker_addr[0] + config.mqtt_broker_addr[1] + config.mqtt_broker_addr[2] + config.mqtt_broker_addr[3]) {
    // establish connection to MQTT broker
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt_client.connected()) {
        // calculate time since last connection attempt
        long now = millis();
        // possible values of mqttLastReconnectAttempt:
        // 0  => never attempted to connect
        // >0 => at least one connect attempt was made
        if ((mqttLastConnectAttempt == 0) || (now - mqttLastConnectAttempt > MQTT_Reconnect_Interval)) {
          mqttLastConnectAttempt = now;
          // attempt to connect
          mqtt_connect();
        }
      } else {
        // client is connected, call the mqtt loop
        mqtt_client.loop();
      }
    }
  }

  // run a CMD whenever a web_cmd event has been triggered
  if (web_cmd != "") {

  iset = true;
  detachInterrupt(RX_PORT); // Interrupt on change of RX_PORT
  delay(1);

    if (web_cmd == "up") {
      cmd_up(web_cmd_channel);
    } else if (web_cmd == "down") {
      cmd_down(web_cmd_channel);
    } else if (web_cmd == "stop") {
      cmd_stop(web_cmd_channel);
    } else if (web_cmd == "set shade") {
      cmd_set_shade_position(web_cmd_channel);
    } else if (web_cmd == "shade") {
      cmd_shade(web_cmd_channel);
    } else if (web_cmd == "learn") {
      cmd_shade(web_cmd_channel);
    } else if (web_cmd == "program") {
      cmd_program(web_cmd_channel);
    } else if (web_cmd == "updown") {
      cmd_updown(web_cmd_channel);
    } else if (web_cmd == "save") {
      Serial.println("main loop: in web_cmd save");
      cmd_save_config();
    } else if (web_cmd == "restart") {
      Serial.println("main loop: in web_cmd restart");
      cmd_restart();
    } else {
      WriteLog("[ERR ] - received unknown command from web_cmd :" + web_cmd, true);
    }
    web_cmd = "";
  }
} // void loop
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// CC1101 radio functions group
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

//####################################################################
// Receive Routine
//####################################################################
void ICACHE_RAM_ATTR radio_rx_measure()
{
  static long LineUp, LineDown, Timeout;
  long LowVal, HighVal;
  int pinstate = digitalRead(RX_PORT); // Read current pin state
  if (micros() - Timeout > 3500) {
    pbwrite = 0;
  }
  if (pinstate)                       // pin is now HIGH, was low
  {
    LineUp = micros();                // Get actual time in LineUp
    LowVal = LineUp - LineDown;       // calculate the LOW pulse time
    if (LowVal < debounce) return;
    if ((LowVal > 300) && (LowVal < 4300))
    {
      if ((LowVal > 3650) && (LowVal < 4300)) {
        Timeout = micros();
        pbwrite = 0;
        lowbuf[pbwrite] = LowVal;
        pbwrite++;
      }
      if ((LowVal > 300) && (LowVal < 1000)) {
        lowbuf[pbwrite] = LowVal;
        pbwrite++;
        Timeout = micros();
      }
    }
  }
  else
  {
    LineDown = micros();          // line went LOW after being HIGH
    HighVal = LineDown - LineUp;  // calculate the HIGH pulse time
    if (HighVal < debounce) return;
    if ((HighVal > 300) && (HighVal < 1000))
    {
      hibuf[pbwrite] = HighVal;
    }
  }
} // void ICACHE_RAM_ATTR radio_rx_measure

//####################################################################
// Generation of the encrypted message (Hopcode)
//####################################################################
void keeloq () {
  Keeloq k(device_key_msb, device_key_lsb);
  unsigned int result = (disc << 16) | jaroliftcnt;  // Append counter value to discrimination value
  dec = k.encrypt(result);
} // void keeloq

//####################################################################
// Keygen generates the device crypt key in relation to the masterkey and provided serial number.
// Here normal key-generation is used according to 00745a_c.PDF Appendix G.
// https://github.com/hnhkj/documents/blob/master/KEELOQ/docs/AN745/00745a_c.pdf
//####################################################################
void keygen () {
  Keeloq k(config.ulMasterMSB, config.ulMasterLSB);
  uint64_t keylow = new_serial | 0x20000000;
  unsigned long enc = k.decrypt(keylow);
  device_key_lsb  = enc;              // Stores LSB devicekey 16Bit
  keylow = new_serial | 0x60000000;
  enc    = k.decrypt(keylow);
  device_key_msb  = enc;              // Stores MSB devicekey 16Bit

  Serial.printf(" created devicekey low: 0x%08x // high: 0x%08x\n", device_key_lsb, device_key_msb);
} // void keygen

//####################################################################
// Add JDE
// Build Somfy frame
//####################################################################
void BuildFrame(byte *frame, uint8_t button,uint32_t REMOTE, unsigned int code) {
  byte checksum;
  
  frame[0] = 0xA7; // Encryption key. Doesn't matter much
  frame[1] = button << 4;  // Which button did  you press? The 4 LSB will be the checksum
  frame[2] = code >> 8;    // Rolling code (big endian)
  frame[3] = code;         // Rolling code
  frame[4] = REMOTE >> 16; // Remote address
  frame[5] = REMOTE >>  8; // Remote address
  frame[6] = REMOTE;       // Remote address

// Checksum calculation: a XOR of all the nibbles
  checksum = 0;
  for(byte i = 0; i < 7; i++) {
    checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
  }
  checksum &= 0b1111; // We keep the last 4 bits only

//Checksum integration
  frame[1] |= checksum; //  If a XOR of all the nibbles is equal to 0, the blinds will
                        // consider the checksum ok.

// Obfuscation: a XOR of all the bytes
  for(byte i = 1; i < 7; i++) {
    frame[i] ^= frame[i-1];
  }
}
//####################################################################
// Add JDE Send Somfy Frames.
//  
//####################################################################
void SendCommand(byte *frame, byte sync) {
  if(sync == 2) { // Only with the first frame.
  //Wake-up pulse & Silence
    digitalWrite(TX_PORT, HIGH);
    delayMicroseconds(9415);
    digitalWrite(TX_PORT, LOW);
    delayMicroseconds(89565);
  }

// Hardware sync: two sync for the first frame, seven for the following ones.
  for (int i = 0; i < sync; i++) {
    digitalWrite(TX_PORT, HIGH);
    delayMicroseconds(4*SYMBOL);
    digitalWrite(TX_PORT, LOW);
    delayMicroseconds(4*SYMBOL);
  }

// Software sync
  digitalWrite(TX_PORT, HIGH);
  delayMicroseconds(4550);
  digitalWrite(TX_PORT, LOW);
  delayMicroseconds(SYMBOL);
    
//Data: bits are sent one by one, starting with the MSB.
  for(byte i = 0; i < 56; i++) {
    if(((frame[i/8] >> (7 - (i%8))) & 1) == 1) {
      digitalWrite(TX_PORT, LOW);
      delayMicroseconds(SYMBOL);
      digitalWrite(TX_PORT, HIGH);
      delayMicroseconds(SYMBOL);
    }
    else {
      digitalWrite(TX_PORT, HIGH);
      delayMicroseconds(SYMBOL);
      digitalWrite(TX_PORT, LOW);
      delayMicroseconds(SYMBOL);
    }
  }
  
  digitalWrite(TX_PORT, LOW);
  delayMicroseconds(30415); // Inter-frame silence
}
//####################################################################
// Simple TX routine. Repetitions for simulate continuous button press.
// Send code two times. In case of one shutter did not "hear" the command.
//####################################################################
void radio_tx(int repetitions) {
  pack = (button << 60) | (new_serial << 32) | dec;
  for (int a = 0; a < repetitions; a++)
  {
    digitalWrite(TX_PORT, LOW);      // CC1101 in TX Mode+
    delayMicroseconds(1150);
    radio_tx_frame(13);              // change 28.01.2018 default 10
    delayMicroseconds(3500);

    for (int i = 0; i < 64; i++) {

      int out = ((pack >> i) & 0x1); // Bitmask to get MSB and send it first
      if (out == 0x1)
      {
        digitalWrite(TX_PORT, LOW);  // Simple encoding of bit state 1
        delayMicroseconds(Lowpulse);
        digitalWrite(TX_PORT, HIGH);
        delayMicroseconds(Highpulse);
      }
      else
      {
        digitalWrite(TX_PORT, LOW);  // Simple encoding of bit state 0
        delayMicroseconds(Highpulse);
        digitalWrite(TX_PORT, HIGH);
        delayMicroseconds(Lowpulse);
      }
    }
    radio_tx_group_h();              // Last 8Bit. For motor 8-16.

    delay(16);                       // delay in loop context is save for wdt
  }
} // void radio_tx

//####################################################################
// Sending of high_group_bits 8-16
//####################################################################
void radio_tx_group_h() {
  for (int i = 0; i < 8; i++) {
    int out = ((disc_h >> i) & 0x1); // Bitmask to get MSB and send it first
    if (out == 0x1)
    {
      digitalWrite(TX_PORT, LOW);    // Simple encoding of bit state 1
      delayMicroseconds(Lowpulse);
      digitalWrite(TX_PORT, HIGH);
      delayMicroseconds(Highpulse);
    }
    else
    {
      digitalWrite(TX_PORT, LOW);    // Simple encoding of bit state 0
      delayMicroseconds(Highpulse);
      digitalWrite(TX_PORT, HIGH);
      delayMicroseconds(Lowpulse);
    }
  }
} // void radio_tx_group_h

//####################################################################
// Generates sync-pulses
//####################################################################
void radio_tx_frame(int l) {
  for (int i = 0; i < l; ++i) {
    digitalWrite(TX_PORT, LOW);
    delayMicroseconds(400);          // change 28.01.2018 default highpulse
    digitalWrite(TX_PORT, HIGH);
    delayMicroseconds(380);          // change 28.01.2018 default lowpulse
  }
} // void radio_tx_frame

//####################################################################
// Calculate device code from received serial number
//####################################################################
void rx_keygen () {
  Keeloq k(config.ulMasterMSB, config.ulMasterLSB);
  uint32_t keylow = rx_serial | 0x20000000;
  unsigned long enc = k.decrypt(keylow);
  rx_device_key_lsb  = enc;        // Stores LSB devicekey 16Bit
  keylow = rx_serial | 0x60000000;
  enc    = k.decrypt(keylow);
  rx_device_key_msb  = enc;        // Stores MSB devicekey 16Bit

  Serial.printf(" received devicekey low: 0x%08x // high: 0x%08x", rx_device_key_lsb, rx_device_key_msb);
} // void rx_keygen

//####################################################################
// Decoding of the hopping code
//####################################################################
void rx_decoder () {
  Keeloq k(rx_device_key_msb, rx_device_key_lsb);
  unsigned int result = rx_hopcode;
  decoded = k.decrypt(result);
  rx_disc_low[0] = (decoded >> 24) & 0xFF;
  rx_disc_low[1] = (decoded >> 16) & 0xFF;
  rx_disc_low[2] = (decoded >> 8) & 0xFF;
  rx_disc_low[3] = decoded & 0xFF;

  Serial.printf(" // decoded: 0x%08x\n", decoded);
} // void rx_decoder

//####################################################################
// calculate RSSI value (Received Signal Strength Indicator)
//####################################################################
void ReadRSSI()
{
  int rssi = 0;
  rssi = ELECHOUSE_cc1101.getRssi();

  Serial.print(" CC1101_RSSI ");
  Serial.println(rssi);
} // void ReadRSSI

//####################################################################
// put CC1101 to receive mode
//####################################################################
void EnterRx() {
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.SetRx();
  mySwitch.enableReceive(RX_PORT);
} // void EnterRx

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// Webserver functions group
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// void html_api() -> see html_api.h

//####################################################################
// convert the file extension to the MIME type
//####################################################################
String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
} // String getContentType

//####################################################################
// send the right file to the client (if it exists)
//####################################################################
bool handleFileRead(String path) {
  if (debug_webui) Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";         // If a folder is requested, send the index file
  String contentType = getContentType(path);            // Get the MIME type
  if (SPIFFS.exists(path)) {                            // If the file exists
    File file = SPIFFS.open(path, "r");                 // Open it
    size_t sent = server.streamFile(file, contentType); // And send it to the client
    file.close();                                       // Then close the file again
    return true;
  }
  if (debug_webui) Serial.println("\tFile Not Found");
  return false;                                         // If the file doesn't exist, return false
} // bool handleFileRead


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// MQTT functions group
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

//####################################################################
// Callback for incoming MQTT messages
//####################################################################
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String remotename;
  if (debug_mqtt) {
    Serial.printf("mqtt in: %s - ", topic);
    for (int i = 0; i < length; i++) {
      Serial.print((char)payload[i]);
    }
    Serial.println();
  }

  // extract channel id from topic name
  int channel = 999;
  char * token = strtok(topic, "/");  // initialize token
  token = strtok(NULL, "/");          // now token = 2nd token
  token = strtok(NULL, "/");          // now token = 3rd token, "shutter" or so
  if (debug_mqtt) Serial.printf("command token: %s\n", token);
  if (strncmp(token, "shutter", 7) == 0) {
    token = strtok(NULL, "/");
    if (token != NULL) {
      channel = atoi(token);
    }
  } else if (strncmp(token, "sendconfig", 10) == 0) {
    WriteLog("[INFO] - incoming MQTT command: sendconfig", true);
    mqtt_send_config();
    return;
  } else if (strncmp(token, "rfsend", 6) == 0) {
    token = strtok(NULL, "/");
    if (token != NULL) {
      remotename=String((char*)token);
    }
    payload[length] = '\0';
  //  String cmd = String((char*)payload);
    WriteLog("[INFO] - incoming MQTT command: rfsend " + remotename + " " + String((char*)payload), true);
    cmd_rfsend(remotename,(char*)payload);
    return;
   } else {
    WriteLog("[ERR ] - incoming MQTT command unknown: " + (String)token, true);
    return;
  }
  

  // convert payload in string
  payload[length] = '\0';
  String cmd = String((char*)payload);

  // print serial message
  WriteLog("[INFO] - incoming MQTT command: channel " + (String) channel + ":", false);
  WriteLog(cmd, true);

  if (channel <= 31) {

    iset = true;
    detachInterrupt(RX_PORT); // Interrupt @Inputpin
    delay(1);

    if (cmd == "UP" || cmd == "0") {
      cmd_up(channel);
    } else if (cmd == "DOWN"  || cmd == "100") {
      cmd_down(channel);
    } else if (cmd == "STOP") {
      cmd_stop(channel);
    } else if (cmd == "SETSHADE") {
      cmd_set_shade_position(channel);
    } else if (cmd == "SHADE" || cmd == "90") {
      cmd_shade(channel);
    } else if (cmd == "LEARN") {
      cmd_learn(channel);
    } else if (cmd == "UPDOWN") {
      cmd_updown(channel);
    } else {
      WriteLog("[ERR ] - incoming MQTT payload unknown.", true);
    }
  } else {
    WriteLog("[ERR ] - invalid channel, choose one of 0-31", true);
  }
} // void mqtt_callback

//####################################################################
// increment and store jaroliftcnt, send jaroliftcnt as mqtt state topic
//####################################################################
void jaroliftcnt_handler(boolean do_increment) {
  if (do_increment)
    jaroliftcnt++;
  EEPROM.put(jaroliftadr, jaroliftcnt);
  EEPROM.commit();
  if (mqtt_client.connected()) {
    String Topic = "stat/" + config.mqtt_devicetopic + "/jaroliftcounter";
    const char * msg = Topic.c_str();
    char jaroliftcntstr[10];
    itoa(jaroliftcnt, jaroliftcntstr, 10);
    mqtt_client.publish(msg, jaroliftcntstr, true);
  }
} // void jaroliftcnt_handler

//####################################################################
// increment and store somfycnt, send somfycnt as mqtt state topic
//####################################################################
void somfycnt_handler(boolean do_increment) {
  if (do_increment)
    somfycnt++;
  EEPROM.put(somfyadr, somfycnt);
  EEPROM.commit();
  if (mqtt_client.connected()) {
    String Topic = "stat/" + config.mqtt_devicetopic + "/somfycounter";
    const char * msg = Topic.c_str();
    char somfycntstr[10];
    itoa(somfycnt, somfycntstr, 10);
    mqtt_client.publish(msg, somfycntstr, true);
  }
} // void somfycnt_handler

//####################################################################
// send status via mqtt
//####################################################################
void mqtt_send_percent_closed_state(int channelNum, int percent, String command) {
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  if (mqtt_client.connected()) {
    char percentstr[4];
    itoa(percent, percentstr, 10);
    String Topic = "stat/" + config.mqtt_devicetopic + "/shutter/" + (String)channelNum;
    const char * msg = Topic.c_str();
    mqtt_client.publish(msg, percentstr);
  }
  WriteLog("[INFO] - command " + command + " for channel " + (String)channelNum + " (" + config.channel_name[channelNum] + ") sent.", true);
} // void mqtt_send_percent_closed_state

//####################################################################
// send remote status via mqtt
//####################################################################
void mqtt_send_remote_state(String remotename, int remotestatus) {
   if (mqtt_client.connected()) {
    String Topic = "tele/" + config.mqtt_devicetopic + "/remote/" + remotename;
    const char * msg = Topic.c_str();
    char remotestatusstr[2];
    itoa(remotestatus, remotestatusstr, 10);
    mqtt_client.publish(msg,remotestatusstr);
  }
  WriteLog("[INFO] - status " + (String)remotestatus + " for remote " + remotename +" sent.", true);
} // void mqtt_send_percent_closed_state

//####################################################################
// send config via mqtt
//####################################################################
void mqtt_send_config() {
  String Payload;
  int configCnt = 0, lineCnt = 0;
  char numBuffer[25];

  if (mqtt_client.connected()) {
    // send config of the shutter channels
    for (int channelNum = 0; channelNum <= 31; channelNum++) {
      if (config.channel_name[channelNum] != "") {
        if (lineCnt == 0) {
          Payload = "{\"channel\":[";
        } else {
          Payload += ", ";
        }
        EEPROM.get(adresses[channelNum], new_serial);
        sprintf(numBuffer, "0x%08x", new_serial);
        Payload += "{\"id\":" + String(channelNum) + ", \"name\":\"" + config.channel_name[channelNum] + "\", "
                   + "\"serial\":\"" + numBuffer +  "\"}";
        lineCnt++;

        if (lineCnt >= 4) {
          Payload += "]}";
          mqtt_send_config_line(configCnt, Payload);
          lineCnt = 0;
        }
      } // if (config.channel_name[channelNum] != "")
    } // for

    // handle last item
    if (lineCnt > 0) {
      Payload += "]}";
      mqtt_send_config_line(configCnt, Payload);
    }

    // send most important other config info
    snprintf(numBuffer, 15, "%d", jaroliftcnt);
    Payload = "{\"serialprefix\":\"" + config.serial + "\", "
              + "\"mqtt-clientid\":\"" + config.mqtt_broker_client_id + "\", "
              + "\"mqtt-devicetopic\":\"" + config.mqtt_devicetopic + "\", "
              + "\"jaroliftcounter\":" + (String)numBuffer + ", "
              + "\"somfycounter\":" + (String)numBuffer + ", "
              + "\"new_learn_mode\":" + (String)config.learn_mode + "}";
    mqtt_send_config_line(configCnt, Payload);
  } // if (mqtt_client.connected())
} // void mqtt_send_config

//####################################################################
// send one config telegram via mqtt
//####################################################################
void mqtt_send_config_line(int & counter, String Payload) {
  String Topic = "stat/" + config.mqtt_devicetopic + "/config/" + (String)counter;
  if (debug_mqtt) Serial.println("mqtt send: " + Topic + " - " + Payload);
  mqtt_client.publish(Topic.c_str(), Payload.c_str());
  counter++;
  yield();
} // void mqtt_send_config_line

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// execute cmd_ functions group
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

//####################################################################
// function to move the shutter up
//####################################################################
void cmd_up(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  if ( channel < 16 ) { 
   EEPROM.get(jaroliftadr, jaroliftcnt);
   ELECHOUSE_cc1101.setMHZ(433.92);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
   ELECHOUSE_cc1101.SetTx();
 
   button = 0x8;
   disc_l = disc_low[channel];
   disc_h = disc_high[channel];
   disc = (disc_l << 8) | (new_serial & 0xFF);
   rx_disc_low[0]  = disc_l;
   rx_disc_high[0] = disc_h;
   keygen();
   keeloq();
   radio_tx(2);
     
   rx_function = 0x8;
   rx_serial_array[0] = (new_serial >> 24) & 0xFF;
   rx_serial_array[1] = (new_serial >> 16) & 0xFF;
   rx_serial_array[2] = (new_serial >> 8) & 0xFF;
   rx_serial_array[3] = new_serial & 0xFF;
   jaroliftcnt_handler(true);
  }
  else
  {
   EEPROM.get(somfyadr, somfycnt);
  // ELECHOUSE_cc1101.Init();
   ELECHOUSE_cc1101.setMHZ(433.42);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
   ELECHOUSE_cc1101.SetTx();
   byte frame[7];
   BuildFrame(frame, 0x2 , new_serial, somfycnt);
   SendCommand(frame, 2);
   for(int i = 0; i<2; i++) {
     SendCommand(frame, 7);
   }
   somfycnt_handler(true);
  } 
  mqtt_send_percent_closed_state(channel, 0, "UP");
  EnterRx();
} // void cmd_up

//####################################################################
// function to move the shutter down
//####################################################################
void cmd_down(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  if ( channel < 16 ) {
   EEPROM.get(jaroliftadr, jaroliftcnt);
   ELECHOUSE_cc1101.setMHZ(433.92);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
   ELECHOUSE_cc1101.SetTx();
   button = 0x2;
   disc_l = disc_low[channel];
   disc_h = disc_high[channel];
   disc = (disc_l << 8) | (new_serial & 0xFF);
   rx_disc_low[0]  = disc_l;
   rx_disc_high[0] = disc_h;
   keygen();
   keeloq();  // Generate encrypted message 32Bit hopcode
   radio_tx(2); // Call TX routine

   rx_function = 0x2;
   rx_serial_array[0] = (new_serial >> 24) & 0xFF;
   rx_serial_array[1] = (new_serial >> 16) & 0xFF;
   rx_serial_array[2] = (new_serial >> 8) & 0xFF;
   rx_serial_array[3] = new_serial & 0xFF;
   jaroliftcnt_handler(true);
  }
  else
  {
   EEPROM.get(somfyadr, somfycnt);
   ELECHOUSE_cc1101.setMHZ(433.42);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
   ELECHOUSE_cc1101.SetTx();
   byte frame[7];
   BuildFrame(frame, 0x4 , new_serial, somfycnt);
   SendCommand(frame, 2);
    for(int i = 0; i<2; i++) {
      SendCommand(frame, 7);
    }
   somfycnt_handler(true);
  } 
  mqtt_send_percent_closed_state(channel, 100, "DOWN");
  EnterRx();
} // void cmd_down

//####################################################################
// function to stop the shutter
//####################################################################
void cmd_stop(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  if ( channel < 16 ) {
   EEPROM.get(jaroliftadr, jaroliftcnt);
   ELECHOUSE_cc1101.setMHZ(433.92);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
   ELECHOUSE_cc1101.SetTx();
   button = 0x4;
   disc_l = disc_low[channel];
   disc_h = disc_high[channel];
   disc = (disc_l << 8) | (new_serial & 0xFF);
   rx_disc_low[0]  = disc_l;
   rx_disc_high[0] = disc_h;
   keygen();
   keeloq();
   radio_tx(2);
 
   rx_function = 0x4;
   rx_serial_array[0] = (new_serial >> 24) & 0xFF;
   rx_serial_array[1] = (new_serial >> 16) & 0xFF;
   rx_serial_array[2] = (new_serial >> 8) & 0xFF;
   rx_serial_array[3] = new_serial & 0xFF;
   jaroliftcnt_handler(true);
  }
  else
  {
   EEPROM.get(somfyadr, somfycnt);
   ELECHOUSE_cc1101.setMHZ(433.42);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
   ELECHOUSE_cc1101.SetTx();
   byte frame[7];
   BuildFrame(frame, 0x1 , new_serial, somfycnt);
   SendCommand(frame, 2);
    for(int i = 0; i<2; i++) {
      SendCommand(frame, 7);
    }
   somfycnt_handler(true);
  } 
  WriteLog("[INFO] - command STOP for channel " + (String)channel + " (" + config.channel_name[channel] + ") sent.", true);
  EnterRx();
} // void cmd_stop

//####################################################################
// function rfsend command
//####################################################################
void cmd_rfsend(String remotename,char* payload) {
  ELECHOUSE_cc1101.setMHZ(433.92); // Here you can set your basic frequency. The lib calculates the frequency automatically (default = 433.92).The cc1101 can: 300-348 MHZ, 387-464MHZ and 779-928MHZ. Read More info from datasheet.
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
  ELECHOUSE_cc1101.SetTx();
  
  mySwitch.setRepeatTransmit(15);

  //Parse Rfsend payload
  char * token = strtok(payload, ",");
  unsigned long command=atol(token);
  token = strtok(NULL, ","); 
  unsigned int bitlength=atoi(token);
  token = strtok(NULL, ","); 
  unsigned int protocol=atoi(token);
  token = strtok(NULL, ","); 
  unsigned int remotestatus=atoi(token);
  
  mySwitch.setProtocol(protocol);
  mySwitch.send(command,bitlength);

  delay(1000);  
  WriteLog("[INFO] - RF command sent: " + (String)command, true);
  mqtt_send_remote_state(remotename, remotestatus);
  EnterRx();
} // void cmd_rfsend

//####################################################################
// function rfreceive command
//####################################################################
void cmd_rfreceive() {
  ELECHOUSE_cc1101.setMHZ(433.92); // Here you can set your basic frequency. The lib calculates the frequency automatically (default = 433.92).The cc1101 can: 300-348 MHZ, 387-464MHZ and 779-928MHZ. Read More info from datasheet.
  ELECHOUSE_cc1101.SetRx();
  mySwitch.enableReceive(5);
 
  uint32_t period =2*60000L;       // 5 minutes
  uint32_t tStart=millis();
  while((millis()-tStart)<period)
   {
   if(mySwitch.available())
    {
     WriteLog("BitLength:"+(String)mySwitch.getReceivedValue(),true);
     WriteLog("BitLrngth:"+(String)mySwitch.getReceivedBitlength(),true );
     WriteLog("Protocol:" + (String)mySwitch.getReceivedProtocol(),true );
     mySwitch.resetAvailable();
    }
  }
  delay(1000);  
  WriteLog("[INFO] - RF command receive: ", true);
} // void cmd_rfsend
//####################################################################
// function to move shutter to shade position
//####################################################################
void cmd_shade(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  if ( channel < 16 ) {
   EEPROM.get(jaroliftadr, jaroliftcnt);
   ELECHOUSE_cc1101.setMHZ(433.92);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
   ELECHOUSE_cc1101.SetTx();
   button = 0x4;
   disc_l = disc_low[channel];
   disc_h = disc_high[channel];
   disc = (disc_l << 8) | (new_serial & 0xFF);
   rx_disc_low[0]  = disc_l;
   rx_disc_high[0] = disc_h;
   keygen();
   keeloq();
   radio_tx(20);
   rx_function = 0x3;
   rx_serial_array[0] = (new_serial >> 24) & 0xFF;
   rx_serial_array[1] = (new_serial >> 16) & 0xFF;
   rx_serial_array[2] = (new_serial >> 8) & 0xFF;
   rx_serial_array[3] = new_serial & 0xFF;
   jaroliftcnt_handler(true);
  }
  else
  {
   EEPROM.get(somfyadr, somfycnt);
   ELECHOUSE_cc1101.setMHZ(433.42);
   ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
   ELECHOUSE_cc1101.SetTx();
   byte frame[7];
   BuildFrame(frame, 0x1 , new_serial, somfycnt);
   SendCommand(frame, 2);
   for(int i = 0; i<2; i++) {
     SendCommand(frame, 7);
   }
   somfycnt_handler(true);
  } 
  mqtt_send_percent_closed_state(channel, 90, "SHADE");
  EnterRx();
} // void cmd_shade

//####################################################################
// function to Set Somfy shutter
//####################################################################
void cmd_program(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  EEPROM.get(somfyadr, somfycnt);
  ELECHOUSE_cc1101.setMHZ(433.42);
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_SOMFY,8);
  ELECHOUSE_cc1101.SetTx();
  byte frame[7];
  BuildFrame(frame, 0x8 , new_serial, somfycnt);
  SendCommand(frame, 2);
  for(int i = 0; i<2; i++) {
   SendCommand(frame, 7);
  }
  somfycnt_handler(true);
  WriteLog("[INFO] - command PROGRAM for channel " + (String)channel + " (" + config.channel_name[channel] + ") sent.", true);
  mqtt_send_percent_closed_state(channel,50 , "PROGRAM");
  EnterRx();
} // void cmd_program

//####################################################################
// function to set the learn/set the shade position
//####################################################################
void cmd_set_shade_position(int channel) {
  EEPROM.get(adresses[channel], new_serial);
  EEPROM.get(jaroliftadr, jaroliftcnt);
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
  ELECHOUSE_cc1101.SetTx();
  
  button = 0x4;
  disc_l = disc_low[channel];
  disc_h = disc_high[channel];
  disc = (disc_l << 8) | (new_serial & 0xFF);
  rx_disc_low[0]  = disc_l;
  rx_disc_high[0] = disc_h;
  keygen();

  for (int i = 0; i < 4; i++) {
    keeloq();
    radio_tx(1);
    jaroliftcnt++;
    delay(300);
  }
  rx_function = 0x6;
  rx_serial_array[0] = (new_serial >> 24) & 0xFF;
  rx_serial_array[1] = (new_serial >> 16) & 0xFF;
  rx_serial_array[2] = (new_serial >> 8) & 0xFF;
  rx_serial_array[3] = new_serial & 0xFF;
  WriteLog("[INFO] - command SET SHADE for channel " + (String)channel + " (" + config.channel_name[channel] + ") sent.", true);
  jaroliftcnt_handler(false);
  delay(2000); // Safety time to prevent accidentally erase of end-points.
  EnterRx();
} // void cmd_set_shade_position

//####################################################################
// function to put the dongle into the learn mode and
// send learning packet.
//####################################################################
void cmd_learn(int channel) {
  WriteLog("[INFO] - putting channel " +  (String) channel + " into learn mode ...", false);
  new_serial = EEPROM.get(adresses[channel], new_serial);
  EEPROM.get(jaroliftadr, jaroliftcnt);
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
  ELECHOUSE_cc1101.SetTx();
  
  if (config.learn_mode == true)
    button = 0xA;                           // New learn method. Up+Down followd by Stop.
  else
    button = 0x1;                           // Old learn method for receiver before Mfg date 2010.
  disc_l = disc_low[channel] ;
  disc_h = disc_high[channel];
  disc = (disc_l << 8) | (new_serial & 0xFF);
  keygen();
  keeloq();
  radio_tx(1);
  jaroliftcnt++;
  delay(300);
  if (config.learn_mode == true) {
    delay(1000);
    button = 0x4;   // Stop
    keeloq();
    radio_tx(1);
    jaroliftcnt++;
  }
  jaroliftcnt_handler(false);
  WriteLog("Channel learned!", true);
  EnterRx();
} // void cmd_learn

//####################################################################
// function to send UP+DOWN button at same time
//####################################################################
void cmd_updown(int channel) {
  new_serial = EEPROM.get(adresses[channel], new_serial);
  EEPROM.get(jaroliftadr, jaroliftcnt);
  ELECHOUSE_cc1101.setMHZ(433.92);
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_PATABLE,PA_JAROLIFT,8);
  ELECHOUSE_cc1101.SetTx();
  
  button = 0xA;
  disc_l = disc_low[channel] ;
  disc_h = disc_high[channel];
  disc = (disc_l << 8) | (new_serial & 0xFF);
  keygen();
  keeloq();
  radio_tx(1);
  jaroliftcnt_handler(true);
  WriteLog("[INFO] - command UPDOWN for channel " + (String)channel + " (" + config.channel_name[channel] + ") sent.", true);
  EnterRx();
} // void cmd_updown

//####################################################################
// webUI save config function
//####################################################################
void cmd_save_config() {
  WriteLog("[CFG ] - save config initiated from WebUI", true);
  // check if mqtt_devicetopic was changed
  if (config.mqtt_devicetopic_new != config.mqtt_devicetopic) {
    // in case the devicetopic has changed, the LWT state with the old devicetopic should go away
    WriteLog("[CFG ] - devicetopic changed, gracefully disconnect from mqtt server", true);
    // first we send an empty message that overwrites the retained "Online" message
    String topicOld = "tele/" + config.mqtt_devicetopic + "/LWT";
    mqtt_client.publish(topicOld.c_str(), "", true);
    // next: remove retained "jaroliftcounter" message
    topicOld = "stat/" + config.mqtt_devicetopic + "/jaroliftcounter";
    mqtt_client.publish(topicOld.c_str(), "", true);
    // next: remove retained "somfycounter" message
    topicOld = "stat/" + config.mqtt_devicetopic + "/somfycounter";
    mqtt_client.publish(topicOld.c_str(), "", true);
    delay(200);
    // finally we disconnect gracefully from the mqtt broker so the stored LWT "Offline" message is discarded
    mqtt_client.disconnect();
    config.mqtt_devicetopic = config.mqtt_devicetopic_new;
    delay(200);
  }
  if (config.set_and_generate_serial) {
    WriteLog("[CFG ] - set and generate new serial, user input: " + config.new_serial, true);
    if ((config.new_serial[0] == '0') && (config.new_serial[1] == 'x')) {
      Serial.println("config.serial is hex");
      // Serial is 28 bits
      // string serial stores only highest 24 bits,
      // add lowest 4 bits with a shift operation for config.serial_number
      config.serial_number = strtoul(config.new_serial.c_str(), NULL, 16) << 4;
      // be safe an convert number back to clean 6-digit hex string
      char serialNumBuffer[11];
        snprintf(serialNumBuffer, 11, "0x%06x", (config.serial_number >> 4));
      config.serial = serialNumBuffer;
      Serial.printf("config.serial: %08u = 0x%08x \n", config.serial_number, config.serial_number);
      cmd_generate_serials(config.serial_number);
    } else {
      server.send ( 200, "text/plain", "Set new Serial not successful, not a hexadecimal number.");
      return;
    }
  }
  if (config.set_jaroliftcounter) {
    uint16_t new_jaroliftcnt = strtoul(config.new_jaroliftcounter.c_str(), NULL, 10);
    WriteLog("[CFG ] - set jaroliftcounter to " + String(new_jaroliftcnt), true);
    jaroliftcnt = new_jaroliftcnt;
    jaroliftcnt_handler(false);
  }
    if (config.set_somfycounter) {
    uint16_t new_somfycnt = strtoul(config.new_somfycounter.c_str(), NULL, 10);
    WriteLog("[CFG ] - set somfycounter to " + String(new_somfycnt), true);
    somfycnt = new_somfycnt;
    somfycnt_handler(false);
  }
  WriteConfig();
  server.send ( 200, "text/plain", "Configuration has been saved, system is restarting. Please refresh manually in about 30 seconds.." );
  cmd_restart();
} // void cmd_save_config

//####################################################################
// webUI restart function
//####################################################################
void cmd_restart() {
  server.send ( 200, "text/plain", "System is restarting. Please refresh manually in about 30 seconds." );
  delay(500);
  wifi_disconnect_log = false;
   ESP.restart();
} // void cmd_restart

//####################################################################
// generates 32 serial numbers
//####################################################################
void cmd_generate_serials(uint32_t sn) {
  WriteLog("[CFG ] - Generate serial numbers starting from" + String(sn), true);
  uint32_t z = sn;
  WriteLog("SN - " + String(sn),true);
  for (uint32_t i = 0; i <= 31; ++i) { // generate 32 serial numbers and storage in EEPROM
    WriteLog("[CFG ] - " + String(i) + " " + String(adresses[i]) + " " + String(z), true);
 
    EEPROM.put(adresses[i], z);   // Serial 4Bytes
    z++;
  }
 // jaroliftcnt = 0;
 // jaroliftcnt_handler(false);
   EEPROM.commit();
  delay(100);
} // void cmd_generate_serials

//####################################################################
// connect function for MQTT broker
// called from the main loop
//####################################################################
boolean mqtt_connect() {
  const char* client_id = config.mqtt_broker_client_id.c_str();
  const char* username = config.mqtt_broker_username.c_str();
  const char* password = config.mqtt_broker_password.c_str();
  String willTopic = "tele/" + config.mqtt_devicetopic + "/LWT"; // connect with included "Last-Will-and-Testament" message
  uint8_t willQos = 0;
  boolean willRetain = true;
  const char* willMessage = "Offline";           // LWT message says "Offline"
  String subscribeString = "cmd/" + config.mqtt_devicetopic + "/#";

  WriteLog("[INFO] - trying to connect to MQTT broker", true);
  // try to connect to MQTT
  if (mqtt_client.connect(client_id, username, password, willTopic.c_str(), willQos, willRetain, willMessage )) {
    WriteLog("[INFO] - MQTT connect success", true);
    // subscribe the needed topics
    mqtt_client.subscribe(subscribeString.c_str());
    // publish telemetry message "we are online now"
    mqtt_client.publish(willTopic.c_str(), "Online", true);
  } else {
    WriteLog("[ERR ] - MQTT connect failed, rc =" + (String)mqtt_client.state(), true);
  }
  return mqtt_client.connected();
} // boolean mqtt_connect

//####################################################################
// NTP update
//####################################################################
void updateNTP() {
  configTime(TIMEZONE * 3600, 0, NTP_SERVERS);
} // void updateNTP

//####################################################################
// callback function when time is set via SNTP
//####################################################################
void time_is_set(void) {
  if (time_is_set_first) {    // call WriteLog only once for the initial time set
    time_is_set_first = false;
    WriteLog("[INFO] - time set from NTP server", true);
  }
} // void time_is_set

