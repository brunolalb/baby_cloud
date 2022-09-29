/*
Smart LED Cloud

Bruno Landau Albrecht
brunolalb@gmail.com

Use at your own will and responsibility

ESP32 is connected to:
* LED Strip (4 LEDs WS2812B)

I use MQTT to report and control this module, so remember to update
  the WiFi network name and password, as well as your MQTT Server address
My MQTT Server runs Mosquitto
I also have a NodeRED MQTT Dashboard for fancy reporting and UI
*/

#include <stdio.h>

// wifi stuff
#include <WiFi.h>
#include <WiFiGeneric.h>
// FreeRTOS - official FreeRTOS lib
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
// mqtt
#include <AsyncMqttClient.h> // https://github.com/marvinroger/async-mqtt-client
// ota
#include <ESPmDNS.h> // comes with the ESP32 lib
#include <AsyncElegantOTA.h> // official lib: AsyncElegantOTA
#include <AsyncTCP.h> // https://github.com/me-no-dev/AsyncTCP
#include <ESPAsyncWebServer.h> // https://github.com/me-no-dev/ESPAsyncWebServer
// led strip - WS2812B
#include <Adafruit_NeoPixel.h>

// debug stuff
SemaphoreHandle_t semph_debug; // controls access to the debug stuff
#define CLOUD_TEST
#define DEBUG


/* WiFi */
#ifdef CLOUD_TEST
#define WIFI_HOSTNAME     "cloud_test"
#else
#define WIFI_HOSTNAME     "cloud2"
#endif
const char* WIFI_SSID = "I Believe Wi Can Fi";
const char* WIFI_PASSWORD = "AlBrEcHt";
TimerHandle_t wifiReconnectTimer;

/* OTA Update */
TimerHandle_t otaReconnectTimer;
AsyncWebServer server(80);

/* RGB LED GPIO pins */
#ifdef CLOUD_TEST
#define LED_R_PIN    21
#define LED_R_ON()   digitalWrite(LED_R_PIN, LOW)
#define LED_R_OFF()  digitalWrite(LED_R_PIN, HIGH)
#define LED_G_PIN    18
#define LED_G_ON()   digitalWrite(LED_G_PIN, LOW)
#define LED_G_OFF()  digitalWrite(LED_G_PIN, HIGH)
#define LED_B_PIN    19
#define LED_B_ON()   digitalWrite(LED_B_PIN, LOW)
#define LED_B_OFF()  digitalWrite(LED_B_PIN, HIGH)
#endif
#define LED_WIFI_PIN    2
#define LED_WIFI_ON()   digitalWrite(LED_WIFI_PIN, HIGH)
#define LED_WIFI_OFF()  digitalWrite(LED_WIFI_PIN, LOW)

/* LED Strip - WS2812B */
#define LED_STRIP_DATA_PIN              13 //D13
#ifdef CLOUD_TEST
#define LED_STRIP_LED_COUNT             10
#else
#define LED_STRIP_LED_COUNT             45
#endif
Adafruit_NeoPixel led_strip = Adafruit_NeoPixel(LED_STRIP_LED_COUNT, LED_STRIP_DATA_PIN, NEO_GRB + NEO_KHZ800);
#define LED_STRIP_INITIAL_BRIGHTNESS    0 // up to LED_STRIP_MAX_BRIGHTNESS
#define LED_STRIP_INITIAL_ONOFF         false
#define LED_STRIP_INITIAL_COLOR_R       0
#define LED_STRIP_INITIAL_COLOR_G       0
#define LED_STRIP_INITIAL_COLOR_B       0

#define LED_STRIP_MAX_VALUE             255
#define LED_STRIP_MAX_BRIGHTNESS        255
typedef struct {
  bool enabled;
  byte r;
  byte g;
  byte b;
  byte brightness;
} pixel_t;
pixel_t pixels_current[LED_STRIP_LED_COUNT];
pixel_t pixels_raw[LED_STRIP_LED_COUNT];
pixel_t pixels_target[LED_STRIP_LED_COUNT];
#define LED_STRIP_UPDATE_PERIOD_MS      50
#define LED_STRIP_SMOOTHNESS_RATE       2


/* MQTT */
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
SemaphoreHandle_t semph_mqtt;
bool mqtt_connected;
// some config
#define MQTT_MY_NAME            WIFI_HOSTNAME
#define MQTT_SERVER_IP          IPAddress(192, 168, 1, 42)
#define MQTT_SERVER_PORT        1883
#define MQTT_PUBLISH_RETAINED   true  // if true, will publish all messages with the retain flag
#define MQTT_PUBLISH_QOS        0     // QOS level for published messages
#define MQTT_SUBSCRIBE_QOS      0     // QOS level for subscribed topics
// topics
#define MQTT_TOPIC_HOME         "smarthome/liam/"
#define MQTT_TOPIC_BASE_ADDRESS MQTT_TOPIC_HOME WIFI_HOSTNAME
#define MQTT_TOPIC_LED_R        MQTT_TOPIC_BASE_ADDRESS "/led/red"      // 1 or 0
#define MQTT_TOPIC_LED_G        MQTT_TOPIC_BASE_ADDRESS "/led/green"    // 1 or 0
#define MQTT_TOPIC_LED_B        MQTT_TOPIC_BASE_ADDRESS "/led/blue"     // 1 or 0

// _f or _F means feedback
#define MQTT_TOPIC_DEBUG              MQTT_TOPIC_BASE_ADDRESS "/debug"            // text
#define MQTT_TOPIC_STRIP_ON_OFF       MQTT_TOPIC_BASE_ADDRESS "/on_off"           // 1 or 0
#define MQTT_TOPIC_STRIP_ON_OFF_FEEDBACK       MQTT_TOPIC_BASE_ADDRESS "/on_off_feedback"  // 1 or 0
#define MQTT_TOPIC_STRIP_R            MQTT_TOPIC_BASE_ADDRESS "/red"              // 0 - 255
#define MQTT_TOPIC_STRIP_G            MQTT_TOPIC_BASE_ADDRESS "/green"            // 0 - 255
#define MQTT_TOPIC_STRIP_B            MQTT_TOPIC_BASE_ADDRESS "/blue"             // 0 - 255
#define MQTT_TOPIC_STRIP_HEX          MQTT_TOPIC_BASE_ADDRESS "/color_hex"        // RGB, hex, 0-FF
#define MQTT_TOPIC_STRIP_BRIGHTNESS   MQTT_TOPIC_BASE_ADDRESS "/brightness"       // 0 - 255
// buffer
char global_buffer[20];


/****************************************
 * Debug
 ****************************************/

#ifdef DEBUG
void debug(char *msg) 
{
  if (xPortInIsrContext()) {
    Serial.print("ISR!!");
    Serial.println(msg);
    return;
  }

  if(xSemaphoreTake(semph_debug, pdMS_TO_TICKS(100)) == pdTRUE ) {
    mqtt_publish(MQTT_TOPIC_DEBUG, msg);
    Serial.println(msg);
    xSemaphoreGive(semph_debug);    
  }  
}

void debug_nonFreeRTOS(char *msg)
{
  Serial.println(msg);
}

void setup_debug()
{
  semph_debug = xSemaphoreCreateMutex();
  xSemaphoreGive(semph_debug);
}
#else
void debug(char *msg) {
}
void debug_nonFreeRTOS(char *msg) {
}
void setup_debug() {
}
#endif



/****************************************
 * WiFi
 ****************************************/

void setup_WiFi() 
{
  char msg[50];
  snprintf(msg, 50, "Connecting to %s", WIFI_SSID);
  debug_nonFreeRTOS(msg);
  
  // delete old config
  WiFi.disconnect(true);

  pinMode(LED_WIFI_PIN, OUTPUT); LED_WIFI_ON();

  delay(1000);

  WiFi.onEvent(WiFiStationStarted, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_START);
  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.mode(WIFI_STA);

  // one shot timer
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)2, reinterpret_cast<TimerCallbackFunction_t>(reconnectToWifi));
}

void reconnectToWifi()
{
  debug("Reconnecting to Wifi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  
}

void WiFiStationStarted(WiFiEvent_t event, WiFiEventInfo_t info)
{
  debug("Station Started");
  WiFi.setHostname(WIFI_HOSTNAME);
}

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  debug("Connected to AP successfully!");
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
  char msg[30];

  LED_WIFI_OFF();

  debug("WiFi connected");
  snprintf(msg, 30, "IP address: %s", WiFi.localIP().toString().c_str());
  debug(msg);
  snprintf(msg, 30, "RRSI: %i dB", WiFi.RSSI());
  debug(msg);

  // connect the mqtt again
  xTimerStart(mqttReconnectTimer, 0);
  // start the OTA again
  xTimerStart(otaReconnectTimer, 0);
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  char msg[50];

  LED_WIFI_ON();

  snprintf(msg, 50, "Disconnected from WiFi: %u", info.wifi_sta_disconnected.reason);
  debug(msg);

  // stop the mqtt reconnect timer to ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  xTimerStop(mqttReconnectTimer, 0); 
  // stop the ota reconnect timer to ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  xTimerStop(otaReconnectTimer, 0); 
  // start the wifi reconnect timer
  xTimerStart(wifiReconnectTimer, 0);
}

/****************************************
 * MQTT
 ****************************************/
void setup_MQTT() 
{
  mqtt_connected = false;

  // one shot timer
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(reconnectToMqtt));

  // semaphore to prevent simultaneous access
  semph_mqtt = xSemaphoreCreateMutex();
  xSemaphoreGive(semph_mqtt);
  
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  //mqttClient.onSubscribe(onMqttSubscribe);
  //mqttClient.onUnsubscribe(onMqttUnsubscribe);
  //mqttClient.onPublish(onMqttPublish);
  mqttClient.onMessage(onMqttMessage); // when we receive a message from a subscribed topic

  mqttClient.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  mqttClient.setClientId(MQTT_MY_NAME);

  // wait for wifi to start, it will start the timer to connect to the mqtt server
}

void reconnectToMqtt() 
{
  debug("Connecting to MQTT...");
  delay(50);
  mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) 
{
  uint16_t packetIdSub;
  char msg[100];

  debug("Connected to MQTT.");
  snprintf(msg, 50, "Session present: %s", sessionPresent ? "True" : "False");
  
  /* subscribe to topics with default QoS 0*/
  #ifdef CLOUD_TEST
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_LED_R, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_LED_R, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_LED_G, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_LED_G, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_LED_B, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_LED_B, packetIdSub);
  debug(msg);
  #endif
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_ON_OFF, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_ON_OFF, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_R, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_R, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_G, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_G, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_B, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_B, packetIdSub);
  debug(msg);
  //packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_HEX, MQTT_SUBSCRIBE_QOS);
  //snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_HEX, packetIdSub);
  //debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_BRIGHTNESS, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_BRIGHTNESS, packetIdSub);
  debug(msg);

  mqtt_connected = true;
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) 
{
  char msg[100];
  
  mqtt_connected = false;

  snprintf(msg, 100, "Disconnected from MQTT. Reason: %u", static_cast<uint8_t>(reason));
  debug(msg);
  
  if (WiFi.isConnected()) {
    // didn't disconnect because the wifi stopped, so try to connect again    
    debug("Wifi still connected.");
    xTimerStart(mqttReconnectTimer, 0);
    debug("Timer restarted.");
  }
}

bool mqtt_publish(char *topic, char *payload)
{
  debug("mqtt_publish");
  if (xSemaphoreTake(semph_mqtt, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (mqtt_connected) {
      mqttClient.publish(topic, MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAINED, payload);
      xSemaphoreGive(semph_mqtt);      
    } else {
      xSemaphoreGive(semph_mqtt);
      return false;
    }
  } else {
    return false;
  }
  return true;
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) 
{
  char msg[200];
  char new_payload[20];
  uint8_t data_u8 = 0;

  snprintf(new_payload, 20, "%s", payload);
  new_payload[len] = 0;
  snprintf(msg, 200, "Message received: %s\r\n payload (%u bytes): %s", 
                     topic, len, new_payload);
  debug(msg);
  
  #ifdef CLOUD_TEST
  if (String(topic) == String(MQTT_TOPIC_LED_R)) {
    (new_payload[0] == '1') ? LED_R_ON() : LED_R_OFF();
  } else if (String(topic) == String(MQTT_TOPIC_LED_G)) {
    (new_payload[0] == '1') ? LED_G_ON() : LED_G_OFF();
  } else if (String(topic) == String(MQTT_TOPIC_LED_B)) {
    (new_payload[0] == '1') ? LED_B_ON() : LED_B_OFF();
  } else 
  #endif
  if (String(topic) == String(MQTT_TOPIC_STRIP_ON_OFF)) {
    setAllPixelsOnOff(new_payload[0] == '1' ? true : false);
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_BRIGHTNESS)) {      
    setAllPixelsBrightness((uint8_t)strtoul((char *)new_payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_R)) {
    setColorAllPixelsRed((uint8_t)strtoul((char *)new_payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_G)) {
    setColorAllPixelsGreen((uint8_t)strtoul((char *)new_payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_B)) {
    setColorAllPixelsBlue((uint8_t)strtoul((char *)new_payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_HEX)) {
    //to RGB
    unsigned long int rgb=strtoul((char *)&new_payload[1], NULL, 16);
    byte r=(byte)((rgb >> 16) & 0xFF);
    byte g=(byte)((rgb >> 8) & 0xFF);
    byte b=(byte) (rgb & 0xFF);
    setColorAllPixels(r, g, b);

    snprintf(msg, 200, "done");
    debug(msg);
    return;

  } else {
    // i don't know ...
    snprintf(msg, 200, "I dont know");
    debug(msg);
    return;
  }

}

/****************************************
 * OTA Updates
 ****************************************/
void reconnectToOta()
{
  debug("reconnectToOta");
  /*use mdns for host name resolution*/
  if (!MDNS.begin(WIFI_HOSTNAME)) { //http://<hostname>.local
    debug("Error setting up MDNS responder!");
    //return;
    /* while (1) {
      delay(1000);
    } */
  }

  AsyncElegantOTA.begin(&server);    // Start ElegantOTA
  debug("Elegant OTA started.");
  
  server.begin();
  debug("Webserver started.");
}

void setup_OTA_Updates()
{

  otaReconnectTimer = xTimerCreate("otaTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)1, reinterpret_cast<TimerCallbackFunction_t>(reconnectToOta));

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Hi! I am ESP32.");
  });

}

/****************************************
 * LED Strip
 ****************************************/

void setup_LED_Strip()
{
  pinMode(LED_STRIP_DATA_PIN, OUTPUT);
  led_strip.begin();
  //led_strip.setBrightness(LED_STRIP_INITIAL_BRIGHTNESS);
  memset(pixels_current, 0, LED_STRIP_LED_COUNT * sizeof(pixel_t));
  memset(pixels_raw, 0, LED_STRIP_LED_COUNT * sizeof(pixel_t));
  memset(pixels_target, 0, LED_STRIP_LED_COUNT * sizeof(pixel_t));
  setColorAllPixels(LED_STRIP_INITIAL_COLOR_R, LED_STRIP_INITIAL_COLOR_G, LED_STRIP_INITIAL_COLOR_B);
  setAllPixelsBrightness(LED_STRIP_INITIAL_BRIGHTNESS);
  setAllPixelsOnOff(LED_STRIP_INITIAL_ONOFF);

  xTaskCreate(LEDStrip_Task,
              (const char *)"LED Strip",
              2048,  // Stack size
              NULL,
              2,  // priority
              NULL );
}

void setColorAllPixels(byte red, byte green, byte blue) 
{
  char msg[100];
  snprintf(msg, 100, "setColorAllPixels: %u, %u, %u", red, green, blue);
  debug(msg);

  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    pixels_target[i].r = red;
    pixels_target[i].g = green;
    pixels_target[i].b = blue;
  }
}

void setColorAllPixelsRed(byte red) 
{
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++)
    pixels_target[i].r = red;
}

void setColorAllPixelsGreen(byte green) 
{
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++)
    pixels_target[i].g = green;
}

void setColorAllPixelsBlue(byte blue) 
{
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++)
    pixels_target[i].b = blue;
}

void setAllPixelsBrightness(byte brightness)
{
  char msg[50];
  snprintf(msg, 50, "setBrightness: %u", brightness);
  debug(msg);

  for (byte i=0; i < LED_STRIP_LED_COUNT; i++)
    pixels_target[i].brightness = brightness;
}

void setAllPixelsOnOff(bool enable)
{
  char msg[50];
  snprintf(msg, 50, "setAllPixelsOnOff: %s", (enable) ? "Enabled" : "Disable");
  debug(msg);
  
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++)
    pixels_target[i].enabled = enable;

  snprintf (msg, 10, "%u", enable ? 1 : 0);
  mqtt_publish(MQTT_TOPIC_STRIP_ON_OFF_FEEDBACK, msg);
}


void LEDStrip_smoothly_match(uint8_t target, uint8_t *current)
{
  uint16_t target16 = (uint16_t)target;
  uint16_t current16 = (uint16_t)*current;

  if (current16 > target16) {
    if (current16 > (target16 + LED_STRIP_SMOOTHNESS_RATE)) {
      current16 -= LED_STRIP_SMOOTHNESS_RATE;
    } else {
      current16 = target16;
    }
  } else if (current16 < target16) {
    if (target16 > (current16 + LED_STRIP_SMOOTHNESS_RATE)) {
      current16 += LED_STRIP_SMOOTHNESS_RATE;
    } else {
      current16 = target16;
    }
  }

  if (current16 > 0x00FF) {
    char msg[200];
    snprintf(msg, 200, "LEDStrip_smoothly_match: weird %u, %lu, %u, %lu", *current, current16, target, target16);
    debug(msg);
    current16 = 0x00FF;
  }

  *current = (uint8_t)current16;
}

uint8_t LEDStrip_get_raw_color(uint8_t value, uint8_t brightness, uint8_t previous)
{
  float valuef = (float)value;
  float brightnessf = (float)brightness;
  float ratio;
  uint8_t new_raw = 0;

  if ((brightness == 0) || (value == 0)) {
    return 0;
  }

  ratio = brightnessf / (float)LED_STRIP_MAX_BRIGHTNESS;
  valuef *= ratio;

  if (valuef < 1) {
    new_raw = 0;
  } else {
    new_raw = int(round(valuef));
  }

  LEDStrip_smoothly_match(new_raw, &previous);

  return previous;
  
}


void LEDStrip_Task(void *pvParameters)
{
  (void) pvParameters;
  TickType_t xLastWakeTime;
  uint8_t red, green, blue;
  char msg[200];

  red = 0;
  green = 0;
  blue = 0;
  
  // the objective is to update the strip with the target values
  xLastWakeTime = xTaskGetTickCount();
  while(1) {
    for (uint8_t i=0; i<LED_STRIP_LED_COUNT; i++) {
      memcpy(&pixels_current[i], &pixels_target[i], sizeof(pixel_t));

      red = LEDStrip_get_raw_color((pixels_current[i].enabled) ? pixels_current[i].r : 0, pixels_current[i].brightness, pixels_raw[i].r);
      green = LEDStrip_get_raw_color((pixels_current[i].enabled) ? pixels_current[i].g : 0, pixels_current[i].brightness, pixels_raw[i].g);
      blue = LEDStrip_get_raw_color((pixels_current[i].enabled) ? pixels_current[i].b: 0, pixels_current[i].brightness, pixels_raw[i].b);

      pixels_raw[i].r = red;
      pixels_raw[i].g = green;
      pixels_raw[i].b = blue;
      snprintf(msg, 200, "task: r %u, g %u, b %u", red, green, blue);
      //debug(msg);
      led_strip.setPixelColor(i, led_strip.Color(pixels_raw[i].r, pixels_raw[i].g, pixels_raw[i].b));
    }

    led_strip.show();

    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(LED_STRIP_UPDATE_PERIOD_MS));
  }

}

/****************************************
 * Application Setup
 ****************************************/

void setup() 
{
  Serial.begin(115200);

  setup_debug();
  
  /* Wifi Setup */
  setup_WiFi();

  /* MQTT Setup */
  setup_MQTT();

  /* OTA Update stuff */
  setup_OTA_Updates();

  // start the wifi reconnect timer
  xTimerStart(wifiReconnectTimer, 0);

  // while we're connecting:
  #ifdef CLOUD_TEST
  /* set led as output to control led on-off */
  pinMode(LED_R_PIN, OUTPUT); LED_R_ON();
  pinMode(LED_G_PIN, OUTPUT); LED_G_OFF();
  pinMode(LED_B_PIN, OUTPUT); LED_B_OFF();
  #endif

  /* LED Strip */
  setup_LED_Strip();
  
}

/****************************************
 * Application Loop
 ****************************************/

void loop() 
{

}
