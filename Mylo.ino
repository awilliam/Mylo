/*
 * Copyright (C) 2017 Alex Williamson, all rights reserved.
 *   Author: Alex Williamson <alex.l.williamson@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <PubSubClient.h>

#define MYLO_MAGIC 0x4d796c6f /* "Mylo" */
/*
 * Bump version to force EEPROM config to invalid, but we have many ways of forcing the config to be
 * re-written without this.
 */
#define MYLO_VER 1

struct MyloConfig_t {
  uint32_t magic;
  uint16_t ver;
  char wifi_ssid[33]; // up to 32
  char wifi_pass[65]; // min 8
  char net_name[33];
  uint8_t http_enable;
  uint16_t http_port;
  uint8_t http_auth;
  char http_user[33];
  char http_pass[33];
  uint8_t mqtt_enable;
  uint16_t mqtt_port;
  char mqtt_host[33];
  char mqtt_pub[33];
  char mqtt_sub[33];
  char mqtt_user[33];
  char mqtt_pass[33];
  uint8_t uart_enable;
  uint32_t uart_baud;
  uint16_t uart_port;
  uint8_t uart_parity;
  uint8_t uart_bits;
  uint8_t uart_stop;
} MyloConfig;

#define MYLO_MODE_CONFIG  0
#define MYLO_MODE_SERVER  1
uint8_t MyloMode;

/*
 * Default UART uses GPIO1(TX) and GPIO3(RX).  Calling Serial.swap() remaps these to GPIO15(TX) and GPIO13(RX).
 * ESP-12E has an LED connected to GPIO16, which is nice for state information.  GPIO14 is immediately adjacent
 * to GND on ESP-12E, making it easy to pull low for configuration with a jumper.  The remainder of the GPIOs are
 * chosen for no special reason.
 */
#define POWER_SWITCH  5
#define RESET_SWITCH  4
#define POWER_STATUS  12
#define CONFIG_SWITCH 14
#define RED_LED       16

WiFiServer *UART_srv;
WiFiClient UART_client;

/* 
 * At 115200n81, we transfer ~12 characters per ms and can therefore fill a 512 byte buffer in ~40ms.  Oversize
 * the buffer so that we hopefully avoid partial reads of the UART, but consider it full and flush out to the
 * client if we cross 512 or 40ms.
 */
#define UART_BUF_FULL 512
#define UART_BUF_SIZE (UART_BUF_FULL + 128)
#define UART_DELAY_MS 40

ESP8266WebServer *Web_srv;

WiFiClient MQTT_client;
PubSubClient MQTT_srv;
unsigned long MQTTConnectTimeout = 0;

/* We can't delay() for the switches, wait for these to timeout instead. */
unsigned long PowerSwitchTimeout = 0;
unsigned long ResetSwitchTimeout = 0;

/*
 * The heartbeat has two modes and we have two different uses for the red LED on the ESP-12E.  When we enter
 * config mode, the LED will do a single pulse heartbeat, as soon as a client connects (1+) it will switch to
 * a double pulse heartbeat.  When in normal server mode, the LED tracks our view of the system power state.
 */
unsigned long HeartbeatTimeout = 0;
uint8_t HeartbeatState = 0;

/* ESP8266 doesn't seem to support EEPROM.update(), so we make our own to only write modified bytes */
void updateConfig() {
  int addr;
  uint8_t *val = (uint8_t *)&MyloConfig;

  for (addr = 0; addr < (int)sizeof(MyloConfig); addr++, val++) {
    if (EEPROM.read(addr) != *val)
      EEPROM.write(addr, *val);
  }
  EEPROM.commit();
}

/* Supply XML list of discoverable access points */
void handleAPList() {
  int i, nets = WiFi.scanNetworks();
  String XML = "<?xml version = \"1.0\" ?>\n<inputs>\n";

  for (i = 0; i < nets; i++) {
    String AP = "<ap>" + WiFi.SSID(i) + "</ap>\n";
    if (XML.indexOf(AP) < 0) {
      XML += AP;
    }
  }
  XML += "</inputs>\n";
  Web_srv->send(200, "text/xml", XML);
}

/*
 * Send our name, in config mode this is "ESP_<ChipId>", in server mode, it's whatevers been set in the config,
 * we don't try to do any DNS resolution.
 */
void handleName() {
  String XML = "<?xml version = \"1.0\" ?>\n<inputs>\n<id>";

  if (MyloMode == MYLO_MODE_CONFIG) {
    String ChipId = String(ESP.getChipId(), HEX);
    ChipId.toUpperCase();
    XML += "ESP_";
    XML += ChipId;
  } else {
    XML += MyloConfig.net_name;
  }
  XML += "</id>\n</inputs>\n";
  Web_srv->send(200, "text/xml", XML);
}

/* Report current power state in XML */
void handlePower() {
  String XML = "<?xml version = \"1.0\" ?>\n<inputs>\n<status>";

  if (digitalRead(POWER_STATUS) == HIGH)
    XML += "ON";
  else
    XML += "OFF";

  XML += "</status>\n</inputs>\n";
  
  Web_srv->send(200, "text/xml", XML);
}

/* Handle valid switch presses.  Do nothing if if doesn't make sense given system power state */
void handlePowerOn() {
  if (digitalRead(POWER_STATUS) == LOW) {
    digitalWrite(POWER_SWITCH, LOW);
    PowerSwitchTimeout = millis() + 500;
  }
  handlePower();
}

void handlePowerOff() {
  if (digitalRead(POWER_STATUS) == HIGH) {
    digitalWrite(POWER_SWITCH, LOW);
    PowerSwitchTimeout = millis() + 5000;
  }
  handlePower();
}

void handleShutdown() {
  if (digitalRead(POWER_STATUS) == HIGH) {
    digitalWrite(POWER_SWITCH, LOW);
    PowerSwitchTimeout = millis() + 500;
  }
  Web_srv->send(200, "text/xml");
}

void handleReset() {
  if (digitalRead(POWER_STATUS) == HIGH) {
    digitalWrite(RESET_SWITCH, LOW);
    ResetSwitchTimeout = millis() + 500;
  }
  Web_srv->send(200, "text/xml");
}

/* Erase EEPROM.  This can be done from either config or server mode.  Server mode causes a restart */
void handleErase() {
  if (MyloMode == MYLO_MODE_SERVER)
    EEPROM.begin(sizeof(MyloConfig));

  memset(&MyloConfig, 0xff, sizeof(MyloConfig));
  updateConfig();
  Web_srv->send(200, "text/xml");

  if (MyloMode == MYLO_MODE_SERVER) {
    EEPROM.end();
    delay(500);
    ESP.restart();
  }
}

/* Trigger a chip resstart */
void handleRestart() {
  Web_srv->send(200, "text/xml");
  delay(500);
  ESP.restart();
}

/* Parse config, save to EEPROM, restart chip */
void handleSave() {
#if 0
  int i;
  for (i = 0; i < Web_srv->args(); i++) {
    Serial.print("  ");
    Serial.print(Web_srv->argName(i));
    Serial.print(" : ");
    Serial.println(Web_srv->arg(i));
  }
#endif

  memset(&MyloConfig, 0, sizeof(MyloConfig));
  MyloConfig.magic = MYLO_MAGIC;
  MyloConfig.ver = MYLO_VER;

  if (Web_srv->arg("SSID"))
    snprintf(MyloConfig.wifi_ssid, sizeof(MyloConfig.wifi_ssid) - 1, "%s", Web_srv->arg("SSID").c_str());
  if (Web_srv->arg("PWD"))
    snprintf(MyloConfig.wifi_pass, sizeof(MyloConfig.wifi_pass) - 1, "%s", Web_srv->arg("PWD").c_str());
  if (Web_srv->arg("NAME"))
    snprintf(MyloConfig.net_name, sizeof(MyloConfig.net_name) - 1, "%s", Web_srv->arg("NAME").c_str());

  if (Web_srv->arg("WEB")) {
    MyloConfig.http_enable = Web_srv->arg("WEB").toInt();
    if (MyloConfig.http_enable) {
      if (Web_srv->arg("WPORT"))
        MyloConfig.http_port = Web_srv->arg("WPORT").toInt();
      else
        MyloConfig.http_port = 80;

      if (Web_srv->arg("WAUTH")) {
        MyloConfig.http_auth = Web_srv->arg("WAUTH").toInt();
        if (MyloConfig.http_auth) {
          if (Web_srv->arg("WUSR"))
            snprintf(MyloConfig.http_user, sizeof(MyloConfig.http_user) - 1, "%s", Web_srv->arg("WUSR").c_str());
          if (Web_srv->arg("WPASS"))
            snprintf(MyloConfig.http_pass, sizeof(MyloConfig.http_pass) - 1, "%s", Web_srv->arg("WPASS").c_str());
        }
      }
    }
  }

  if (Web_srv->arg("MQTT")) {
    MyloConfig.mqtt_enable = Web_srv->arg("MQTT").toInt();
    if (MyloConfig.mqtt_enable) {
      if (Web_srv->arg("MHOST"))
        snprintf(MyloConfig.mqtt_host, sizeof(MyloConfig.mqtt_host) - 1, "%s", Web_srv->arg("MHOST").c_str());
      if (Web_srv->arg("MPUB"))
        snprintf(MyloConfig.mqtt_pub, sizeof(MyloConfig.mqtt_pub) - 1, "%s", Web_srv->arg("MPUB").c_str());
      if (Web_srv->arg("MSUB"))
        snprintf(MyloConfig.mqtt_sub, sizeof(MyloConfig.mqtt_sub) - 1, "%s", Web_srv->arg("MSUB").c_str());
      if (Web_srv->arg("MUSR"))
        snprintf(MyloConfig.mqtt_user, sizeof(MyloConfig.mqtt_user) - 1, "%s", Web_srv->arg("MUSR").c_str());
      if (Web_srv->arg("MPASS"))
        snprintf(MyloConfig.mqtt_pass, sizeof(MyloConfig.mqtt_pass) - 1, "%s", Web_srv->arg("MPASS").c_str());
      if (Web_srv->arg("MPORT"))
        MyloConfig.mqtt_port = Web_srv->arg("MPORT").toInt();
      else
        MyloConfig.mqtt_port = 1883;
    }
  }

  if (Web_srv->arg("UART")) {
    MyloConfig.uart_enable = Web_srv->arg("UART").toInt();
    if (MyloConfig.uart_enable) {
      if (Web_srv->arg("BAUD"))
        MyloConfig.uart_baud = Web_srv->arg("BAUD").toInt();

      MyloConfig.uart_parity = UART_PARITY_NONE;
      if (Web_srv->arg("PARITY")) {
        String parity = Web_srv->arg("PARITY");
        if (parity.equals("Even"))
          MyloConfig.uart_parity = UART_PARITY_EVEN;
        else if (parity.equals("Odd"))
          MyloConfig.uart_parity = UART_PARITY_ODD;
      }
      
      MyloConfig.uart_bits = UART_NB_BIT_8;
      if (Web_srv->arg("BITS")) {
        String bits = Web_srv->arg("BITS");
        if (bits.equals("7"))
          MyloConfig.uart_bits = UART_NB_BIT_7;
        else if (bits.equals("6"))
          MyloConfig.uart_bits = UART_NB_BIT_6;
        else if (bits.equals("5"))
          MyloConfig.uart_bits = UART_NB_BIT_5;
      }

      MyloConfig.uart_stop = UART_NB_STOP_BIT_1;
      if (Web_srv->arg("STOP")) {
        String stp = Web_srv->arg("STOP");
        if (stp.equals("1.5"))
          MyloConfig.uart_stop = UART_NB_STOP_BIT_15;
        else if (stp.equals("2"))
          MyloConfig.uart_stop = UART_NB_STOP_BIT_2;
      }

      if (Web_srv->arg("PORT"))
        MyloConfig.uart_port = Web_srv->arg("PORT").toInt();
      else
        MyloConfig.uart_port = 9999;
    }
  }

  Web_srv->send(200, "text/xml");

  updateConfig();
  EEPROM.end();
  delay(500);
  ESP.restart();
}

void handleFavicon() {
  File favicon = SPIFFS.open("/favicon.ico", "r");
  Web_srv->streamFile(favicon, "image/x-icon");
}

/* Toggle the red LED in server mode */
void powerChange() {
  digitalWrite(RED_LED, !digitalRead(POWER_STATUS));
}

/* We currently only publish ON/OFF */
void publishStatus(boolean force = false) {
  static int lastPublished = -1;
  int currentStatus = digitalRead(POWER_STATUS);

  if (MyloConfig.mqtt_pub[0] && (currentStatus != lastPublished || force)) {
    if (MQTT_srv.publish(MyloConfig.mqtt_pub, currentStatus ? "ON" : "OFF"))
      lastPublished = currentStatus;
  }
}

/* But we allow full power control and query status */
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  int currentStatus = digitalRead(POWER_STATUS);
  char cmd[10] = { 0 };

  memcpy(cmd, payload, length > sizeof(cmd) - 1 ? sizeof(cmd) - 1 : length);

  String Cmd(cmd);
  Cmd.toUpperCase();  /* Be lazy and use String class to convert to upper */

  if (Cmd.equals("ON")) {
    if (!currentStatus) {
      digitalWrite(POWER_SWITCH, LOW);
      PowerSwitchTimeout = millis() + 500;
    }
  } else if (Cmd.equals("OFF")) {
    if (currentStatus) {
      digitalWrite(POWER_SWITCH, LOW);
      PowerSwitchTimeout = millis() + 5000;
    }    
  } else if (Cmd.equals("SHUTDOWN")) {
    if (currentStatus) {
      digitalWrite(POWER_SWITCH, LOW);
      PowerSwitchTimeout = millis() + 500;
    }        
  } else if (Cmd.equals("RESET")) {
    if (currentStatus) {
      digitalWrite(RESET_SWITCH, LOW);
      ResetSwitchTimeout = millis() + 500;
    }
  } else if (Cmd.equals("STATUS")) {
    publishStatus(true);
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(sizeof(MyloConfig));
  EEPROM.get(0, MyloConfig);
  WiFi.softAPdisconnect(true); /* Lingers otherwise */
  
  Serial.println("");

  pinMode(POWER_SWITCH, OUTPUT);
  digitalWrite(POWER_SWITCH, HIGH);

  pinMode(RESET_SWITCH, OUTPUT);
  digitalWrite(RESET_SWITCH, HIGH);

  pinMode(POWER_STATUS, INPUT); /* Use external pulldown here */
  pinMode(CONFIG_SWITCH, INPUT_PULLUP); /* Will float and occasionally force us into config if not pullup */
  pinMode(RED_LED, OUTPUT);

  /* AFAICT, there's no end() method.  We don't write to SPIFFS anyway */
  if (!SPIFFS.begin()) {
    Serial.println("Fatal error, failed to mount SPIFFS, unable to proceed");
    while (1)
      delay(1000);
  }

  MyloMode = MYLO_MODE_CONFIG; /* Until proven otherwise */
  
  if (MyloConfig.magic != MYLO_MAGIC || MyloConfig.ver != MYLO_VER ||
      (MyloConfig.http_enable | MyloConfig.mqtt_enable | MyloConfig.uart_enable) == 0 ||
      !MyloConfig.wifi_ssid[0]) {
    Serial.println("Mylo config invalid, entering setup");
  } else if (digitalRead(CONFIG_SWITCH) == LOW) {
    Serial.println("Mylo config requested, entering setup");
  } else {
    uint8_t i;
    
    Serial.print("Mylo connecting to ");
    Serial.print(MyloConfig.wifi_ssid);

    if (MyloConfig.net_name[0])
      WiFi.hostname(MyloConfig.net_name);

    if (MyloConfig.wifi_pass[0])
      WiFi.begin(MyloConfig.wifi_ssid, MyloConfig.wifi_pass);
    else
      WiFi.begin(MyloConfig.wifi_ssid);
      
    for (i = 0; i < 60; i++) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("");
        Serial.print("Connected, IP address ");
        Serial.println(WiFi.localIP());
        MyloMode = MYLO_MODE_SERVER;
        EEPROM.end();
        break;
      }
      Serial.print(".");
      delay(500);
    }
  }

  if (MyloMode == MYLO_MODE_SERVER) {
    if (MyloConfig.uart_enable) {
      UART_srv = new WiFiServer(MyloConfig.uart_port);
      UART_srv->begin();
      UART_srv->setNoDelay(true);
      Serial.print("Serial server on port ");
      Serial.println(MyloConfig.uart_port);
    }

    if (MyloConfig.http_enable) {
      Web_srv = new ESP8266WebServer(MyloConfig.http_port);
      Web_srv->serveStatic("/", SPIFFS, "/power.html");
      Web_srv->on("/favicon.ico", handleFavicon);
      Web_srv->on("/power.xml", handlePower);
      Web_srv->on("/poweron.xml", handlePowerOn);
      Web_srv->on("/poweroff.xml", handlePowerOff);
      Web_srv->on("/shutdown.xml", handleShutdown);
      Web_srv->on("/reset.xml", handleReset);
      Web_srv->on("/restart.xml", handleRestart);
      Web_srv->on("/erase.xml", handleErase);
      Web_srv->on("/name.xml", handleName);
      Web_srv->begin();
      Serial.print("Web server on port ");
      Serial.println(MyloConfig.http_port);
    }

    if (MyloConfig.mqtt_enable) {
      MQTT_srv.setClient(MQTT_client);
      Serial.print("MQTT server: ");
      Serial.print(MyloConfig.mqtt_host);
      Serial.print(":");
      Serial.println(MyloConfig.mqtt_port);
      MQTT_srv.setServer(MyloConfig.mqtt_host, MyloConfig.mqtt_port);
      MQTT_srv.setCallback(callbackMQTT);
      Serial.print("  Subscribing to: ");
      Serial.println(MyloConfig.mqtt_sub);
      Serial.print("  Publishing to:  ");
      Serial.println(MyloConfig.mqtt_pub);      
    }

    digitalWrite(RED_LED, !digitalRead(POWER_STATUS));
    attachInterrupt(digitalPinToInterrupt(POWER_STATUS), powerChange, CHANGE);
  } else if (MyloMode == MYLO_MODE_CONFIG) {
    String SSID = String(ESP.getChipId(), HEX);
    SSID.toUpperCase();
    SSID = "Mylo@ESP_" + SSID;
    Serial.print("Enabling configuration web server...");
    if (WiFi.softAP(SSID.c_str(), "SetupMyMylo"))
      Serial.println("done");
    else {
      Serial.println("Failed");
      while (1)
        delay(1000);
    }
    
    Web_srv = new ESP8266WebServer(80);
    Web_srv->serveStatic("/", SPIFFS, "/config.html");
    Web_srv->on("/favicon.ico", handleFavicon);
    Web_srv->on("/aplist.xml", handleAPList);
    Web_srv->on("/name.xml", handleName);
    Web_srv->on("/erase.xml", handleErase);
    Web_srv->on("/restart.xml", handleRestart);
    Web_srv->on("/save.xml", handleSave);
    /* Local power control even in config mode... tricky.  Yes, the jquery css is broken, deal. */
    Web_srv->serveStatic("/power.html", SPIFFS, "/power.html");
    Web_srv->on("/power.xml", handlePower);
    Web_srv->on("/poweron.xml", handlePowerOn);
    Web_srv->on("/poweroff.xml", handlePowerOff);
    Web_srv->on("/shutdown.xml", handleShutdown);
    Web_srv->on("/reset.xml", handleReset);
    Web_srv->begin();
  
    Serial.print("Connect to wireless network \"");
    Serial.print(SSID);
    Serial.println("\" with passphrase \"SetupMyMylo\"");
    Serial.print("Use a web browser to access http://");
    Serial.print(WiFi.softAPIP());
    Serial.println(" to configure Mylo");
    HeartbeatTimeout = millis();
  }

  /*
   * Beyond this point in server mode, with serial server enabled, the UART belongs to the serial server.
   * Theoretically there's another UART that only has TX, but we can't use it after the swap.
   */
  if (MyloMode == MYLO_MODE_SERVER && MyloConfig.uart_enable) {
    Serial.flush();
    Serial.begin(MyloConfig.uart_baud,
                 (SerialConfig)(MyloConfig.uart_parity | MyloConfig.uart_bits | MyloConfig.uart_stop));
    Serial.swap();
  }
}

/*
 * ESP8266 doesn't support serialEvent() :(
 * We only support a single client, old connections get kicked.
 */
void handleSerial() {
  if (UART_client && !UART_client.connected())
      UART_client.stop();
      
  if (UART_srv->hasClient()){
    if (UART_client)
      UART_client.stop();
    UART_client = UART_srv->available();
  }
  
  if (UART_client) {
    static uint8_t UART_buf[UART_BUF_SIZE];
    static size_t UART_idx = 0;
    static unsigned long UART_timeout = 0;
    size_t UART_avail;

    if (UART_client.available())
      Serial.write(UART_client.read());

    UART_avail = Serial.available();
    if (UART_avail) {
      /* min() */
      UART_avail = UART_avail > UART_BUF_SIZE - UART_idx ? UART_BUF_SIZE - UART_idx : UART_avail;
      if (UART_avail) {
        UART_avail = Serial.readBytes(&UART_buf[UART_idx], UART_avail);
        if (!UART_idx) /* Start the send timer when we fill the head */
          UART_timeout = millis() + UART_DELAY_MS;
        UART_idx += UART_avail;
      }
    }
    
    if (UART_idx && (UART_idx >= UART_BUF_FULL || (long)(millis() - UART_timeout) >= 0)) {
      size_t wrote = UART_client.write((const uint8_t *)UART_buf, UART_idx);
      if (wrote != UART_idx) { /* Not sure if possible, but handle a short write */
        memmove(UART_buf, &UART_buf[wrote], UART_idx - wrote);
        UART_idx -= wrote;
        UART_timeout = millis() + UART_DELAY_MS;
      } else {
        UART_idx = 0;
      }
    }
  }  
}

void reconnectMQTT() {
  boolean connected;
  
  if (MyloConfig.mqtt_user[0] && MyloConfig.mqtt_pass[0])
    connected = MQTT_srv.connect(MyloConfig.net_name, MyloConfig.mqtt_user, MyloConfig.mqtt_pass);
  else
    connected = MQTT_srv.connect(MyloConfig.net_name);

  if (connected) {
    if (MyloConfig.mqtt_sub[0])
      MQTT_srv.subscribe(MyloConfig.mqtt_sub);
  } else {
    /* Retry gently, 5s interval */
    MQTTConnectTimeout = millis() + 5000;
  }
}

void handleMQTT() {
  if (!MQTT_srv.connected() && (long)(millis() - MQTTConnectTimeout) >= 0)
    reconnectMQTT();
    
  if (MQTT_srv.connected()) {
    publishStatus();
    MQTT_srv.loop();
  }
}

void loop() {
  if (MyloMode == MYLO_MODE_SERVER) {
    if (MyloConfig.uart_enable)
      handleSerial();
    if (MyloConfig.http_enable)
      Web_srv->handleClient();
    if (MyloConfig.mqtt_enable)
      handleMQTT();
  } else if (MyloMode == MYLO_MODE_CONFIG) {
    Web_srv->handleClient();

    /* Do config mode hearbeat */
    if ((long)(millis() - HeartbeatTimeout) >= 0) {
      digitalWrite(RED_LED, HeartbeatState % 2 ? LOW : HIGH);
      HeartbeatTimeout = millis() + (HeartbeatState % (WiFi.softAPgetStationNum() > 0 ? 4 : 2) == 0 ? 1500 : 200);
      HeartbeatState++;
    }
  }

  /* Always handle re-asserting switch GPIOs, can be toggled in either mode. */
  if (digitalRead(POWER_SWITCH) == LOW && (long)(millis() - PowerSwitchTimeout) >= 0)
    digitalWrite(POWER_SWITCH, HIGH);
  if (digitalRead(RESET_SWITCH) == LOW && (long)(millis() - ResetSwitchTimeout) >= 0)
    digitalWrite(RESET_SWITCH, HIGH);
}
