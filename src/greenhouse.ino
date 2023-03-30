#include <Arduino.h>
#include <ESP8266WiFi.h>
#define MQTT_MAX_PACKET_SIZE 256
#include <WiFiClientSecure.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <SHT1x-ESP.h>
#include "credentials.h"
#include "config.h"

const char *hostname = WIFI_HOSTNAME;
const char *ssid = WIFI_SSID;
const char *password =  WIFI_PASSWORD;
const char *mqttServer = MQTT_SERVER;
const int mqttPort = MQTT_PORT;
const char *mqttUser = MQTT_USERNAME;
const char *mqttPassword = MQTT_PASSWORD;
const char *mqttID = MQTT_ID;
const int relayPump = RELAY_PUMP;
const int relayLight = RELAY_LIGHT;
const int dataPin = SDA_PIN;
const int clockPin = SCL_PIN;

unsigned long heartbeat_previousMillis = 0;
const long heartbeat_interval = HEARTBEAT_INTERVALL;

unsigned long sensor_previousMillis = 0;
const long sensor_interval = SENSOR_INTERVALL;
unsigned long emergencystop_previousMillis = 0;
const long emergencystop_threshold = EMERGENCYSTOP_THRESHOLD;
bool emergencystop_running = false;
float temperature_local;
float humidity_local;

// default to 5.0v boards, e.g. Arduino UNO
// SHT1x sht1x(dataPin, clockPin);
// if 3.3v board is used (recommended)
SHT1x sht1x(dataPin, clockPin, SHT1x::Voltage::DC_3_3v);

WiFiClientSecure espClient;
PubSubClient client(espClient);
 
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(relayPump, HIGH);
  pinMode(relayPump, OUTPUT);
  digitalWrite(relayLight, HIGH);
  pinMode(relayLight, OUTPUT);
  /** https://www.sparkfun.com/datasheets/Sensors/SHT1x_datasheet.pdf
      To avoid signal contention the microcontroller must only drive DATA (dataPin) low. An external pull-up resistor (e.g. 10kΩ) 
      is required to pull the signal high – it should be noted that pull-up resistors may be included in I/O circuits of
      microcontrollers. **/
  pinMode(dataPin, INPUT_PULLUP);
  pinMode(clockPin, INPUT);
  espClient.setInsecure();
  reconnect();
}

void reconnect() {
  while (!client.connected()) {
    WiFi.mode(WIFI_STA);
	  WiFi.hostname(hostname);
    delay(100);
    Serial.println();
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    Serial.println("Connected to WiFi network");
    Serial.print("  SSID: ");
    Serial.print(ssid);
    Serial.print(" / Channel: ");
    Serial.println(WiFi.channel());
    Serial.print("  IP Address: ");
    Serial.print(WiFi.localIP());
    Serial.print(" / Subnet Mask: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("  Gateway: ");
    Serial.print(WiFi.gatewayIP());
    Serial.print(" / DNS: ");
    Serial.print(WiFi.dnsIP());
    Serial.print(", ");
    Serial.println(WiFi.dnsIP(1));
    Serial.println("");

    // https://pubsubclient.knolleary.net/api.html
    client.setServer(mqttServer, mqttPort);
    client.setCallback(callback);
    Serial.println("Connecting to MQTT broker");
    Serial.print("  MQTT Server: ");
    Serial.println(mqttServer);
    Serial.print("  MQTT Port: ");
    Serial.println(mqttPort);
    Serial.print("  MQTT Username: ");
    Serial.println(mqttUser);
    Serial.print("  MQTT Identifier: ");
    Serial.println(mqttID);
    Serial.println("");

    while (!client.connected()) {
      if (client.connect(mqttID, mqttUser, mqttPassword)) {
        Serial.println("Connected to MQTT broker");
        Serial.println("Subscribe MQTT Topics");
        client.subscribe("home/outdoor/greenhouse/light");
        client.subscribe("home/outdoor/greenhouse/pump");
        Serial.println("");
        digitalWrite(LED_BUILTIN, HIGH); 
       } else {
        Serial.print("Connection to MQTT broker failed with state: ");
        Serial.println(client.state());
        char puffer[100];
        espClient.getLastSSLError(puffer,sizeof(puffer));
        Serial.print("TLS connection failed with state: ");
        Serial.println(puffer);
        Serial.println("");
        digitalWrite(relayPump, HIGH);
        delay(4000);
       }
    }
  }
}

// Function to receive MQTT messages
void mqttloop() {
  if (!client.loop())
    client.connect(mqttID);
}

// Function to send MQTT messages
void mqttsend(const char *_topic, const char *_data) {
  client.publish(_topic, _data);
}

// Pointer to a message callback function called when a message arrives for a subscription created by this client.
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message topic: ");
  Serial.print(topic);
  Serial.print(" | Message Payload: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println("");
  setCisternStatus(topic, payload, length);  
}

void loop() {
  client.loop();
  reconnect();
  sensor();
  heartbeat();
  switchontime();
  emergencystop();
  mqttloop();
}

void sensor() {
  unsigned long sensor_currentMillis = millis();
  if (sensor_currentMillis - sensor_previousMillis >= sensor_interval) {
    sensor_previousMillis = sensor_currentMillis;
    readsensor_sht10();
    Serial.println("");
  }
}

bool emergencystop_running_prev = false;
unsigned long emergencystop_switchon = 0;

void switchontime() {
  if (!emergencystop_running_prev && emergencystop_running) {
    emergencystop_switchon = millis();
    emergencystop_running_prev = emergencystop_running;
  }
}

unsigned long timeDeltaOld = 0;
void emergencystop() {
  String pinStatus;
  if (emergencystop_running) {
    unsigned long emergencystop_currentMillis = millis();
    
    if (emergencystop_currentMillis - emergencystop_switchon >= emergencystop_threshold) {
      Serial.println("Emergency STOP greenhouse pump");
      digitalWrite(relayPump, HIGH);
      pinStatus = digitalRead(relayPump);
      Serial.print("Status of GPIO pin ");
      Serial.print(relayPump);
      Serial.print(" is ");
      Serial.println(pinStatus);
      delay(1000);
      Serial.println("Send Emergency STOP signal to MQTT broker");
      Serial.println("");
      client.publish("home/outdoor/greenhouse/emergencystop", "on");
      client.publish("home/outdoor/greenhouse/pump", "off");
      emergencystop_running = false;
      emergencystop_running_prev = false;
    }
  }
}

void heartbeat() {
  unsigned long heartbeat_currentMillis = millis();
  if (heartbeat_currentMillis - heartbeat_previousMillis >= heartbeat_interval) {
    heartbeat_previousMillis = heartbeat_currentMillis;
    Serial.println("Send heartbeat signal to MQTT broker");
    Serial.println("");
    client.publish("home/outdoor/greenhouse/heartbeat", "on");
  }
}

void setCisternStatus(char* topic, byte* payload, unsigned int length) {
  String mqttTopic = String(topic);
  String mqttPayload;
  for (unsigned int i = 0; i < length; i++) {
    mqttPayload += (char)payload[i];
    }
  String pinStatus;

  if (mqttTopic == "home/outdoor/greenhouse/pump")
  {
    if (mqttPayload == "on") {
      Serial.println("Switch on greenhouse pump");
      digitalWrite(relayPump, LOW);
      int pinStatus = digitalRead(relayPump);
      Serial.print("Status of GPIO pin ");
      Serial.print(relayPump);
      Serial.print(" is ");
      Serial.println(pinStatus);
      client.publish("home/outdoor/greenhouse/pump/response", "on");
      delay(1000);
      emergencystop_running = true;
      emergencystop_running_prev = false;
    } else if (mqttPayload == "off") {
      Serial.println("Switch off greenhouse pump");
      digitalWrite(relayPump, HIGH);
      int pinStatus = digitalRead(relayPump);
      Serial.print("Status of GPIO pin ");
      Serial.print(relayPump);
      Serial.print(" is ");
      Serial.println(pinStatus);
      client.publish("home/outdoor/greenhouse/pump/response", "off");
      delay(1000);
      emergencystop_running = false;
      emergencystop_running_prev = false;
    } else {
      Serial.println("No valid mqtt command");
    }
  }

  else if (mqttTopic == "home/outdoor/greenhouse/light")
  {
    if (mqttPayload == "on") {
      Serial.println("Switch on light");
      digitalWrite(relayLight, LOW);
      int pinStatus = digitalRead(relayLight);
      Serial.print("Status of GPIO pin ");
      Serial.print(relayLight);
      Serial.print(" is ");
      Serial.println(pinStatus);
      client.publish("home/outdoor/greenhouse/light/response", "on");
      delay(1000);
    } else if (mqttPayload == "off") {
      Serial.println("Switch off light");
      digitalWrite(relayLight, HIGH);
      int pinStatus = digitalRead(relayLight);
      Serial.print("Status of GPIO pin ");
      Serial.print(relayLight);
      Serial.print(" is ");
      Serial.println(pinStatus);
      client.publish("home/outdoor/greenhouse/light/response", "off");
      delay(1000);
    } else {
      Serial.println("No valid mqtt command");
    }
  }
  Serial.println("");  
}

void readsensor_sht10() {
  temperature_local = sht1x.readTemperatureC();
  Serial.print("Temperature: ");
  Serial.print(temperature_local, DEC);
  Serial.println(" *C");
  static char temperature_local_char[7];
  dtostrf(temperature_local, 1, 2, temperature_local_char);
  Serial.print("  MQTT publish home/outdoor/greenhouse/temperature: ");
  Serial.println(temperature_local_char);
  client.publish("home/outdoor/greenhouse/temperature", temperature_local_char, true);
  delay(100);

  humidity_local = sht1x.readHumidity();
  Serial.print("Humidity: ");
  Serial.print(humidity_local);
  Serial.println(" %");
  static char humidity_local_char[7];
  dtostrf(humidity_local, 1, 2, humidity_local_char);
  Serial.print("  MQTT publish home/outdoor/greenhouse/humidity: ");
  Serial.println(humidity_local_char);
  client.publish("home/outdoor/greenhouse/humidity", humidity_local_char, true);
  delay(100);
}