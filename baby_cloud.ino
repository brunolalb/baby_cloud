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
#define LED_STRIP_INITIAL_BRIGHTNESS    10 // up to 255
#define LED_STRIP_INITIAL_ONOFF         false
#define LED_STRIP_INITIAL_COLOR_R       255
#define LED_STRIP_INITIAL_COLOR_G       255
#define LED_STRIP_INITIAL_COLOR_B       255
typedef struct {
  bool enabled;
  byte r;
  byte g;
  byte b;
  byte brightness;
} pixel_t;
pixel_t pixels[LED_STRIP_LED_COUNT];


/* MQTT */
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
SemaphoreHandle_t semph_mqtt;
// some config
#define MQTT_MY_NAME      WIFI_HOSTNAME
#define MQTT_SERVER_IP    IPAddress(192, 168, 1, 42)
#define MQTT_SERVER_PORT  1883
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

  WiFi.onEvent(WiFiStationStarted, SYSTEM_EVENT_STA_START);
  WiFi.onEvent(WiFiStationConnected, SYSTEM_EVENT_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);

  WiFi.mode(WIFI_STA);

  // one shot timer
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(reconnectToWifi));
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

  snprintf(msg, 50, "Disconnected from WiFi: %u", info.disconnected.reason);
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
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_HEX, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_HEX, packetIdSub);
  debug(msg);
  packetIdSub = mqttClient.subscribe(MQTT_TOPIC_STRIP_BRIGHTNESS, MQTT_SUBSCRIBE_QOS);
  snprintf(msg, 100, "Subscribing at QoS %u, topic %s, packetId %u", MQTT_SUBSCRIBE_QOS, MQTT_TOPIC_STRIP_BRIGHTNESS, packetIdSub);
  debug(msg);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) 
{
  debug("Disconnected from MQTT.");
  if (WiFi.isConnected()) {
    // didn't disconnect because the wifi stopped, so try to connect again    
    xTimerStart(mqttReconnectTimer, 0);
  }
}

bool mqtt_publish(char *topic, char *payload)
{
  if (xSemaphoreTake(semph_mqtt, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (mqttClient.connected()) {
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
  snprintf(new_payload, 20, "%s", payload);
  new_payload[len] = 0;
  snprintf(msg, 200, "Message received: %s\r\n payload (%u bytes): %s", 
                     topic, len, new_payload);
  debug(msg);
  
  #ifdef CLOUD_TEST
  if (String(topic) == String(MQTT_TOPIC_LED_R)) {
    (payload[0] == '1') ? LED_R_ON() : LED_R_OFF();
  } else if (String(topic) == String(MQTT_TOPIC_LED_G)) {
    (payload[0] == '1') ? LED_G_ON() : LED_G_OFF();
  } else if (String(topic) == String(MQTT_TOPIC_LED_B)) {
    (payload[0] == '1') ? LED_B_ON() : LED_B_OFF();
  } else 
  #endif
  if (String(topic) == String(MQTT_TOPIC_STRIP_ON_OFF)) {
    setAllPixelsOnOff(payload[0] == '1' ? true : false);
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_BRIGHTNESS)) {
    payload[len] = 0;
    setAllPixelsBrightness(strtol((char *)payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_R)) {
    setColorAllPixelsRed(strtol((char *)payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_G)) {
    setColorAllPixelsGreen(strtol((char *)payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_B)) {
    setColorAllPixelsBlue(strtol((char *)payload, NULL, 10));
  } else if (String(topic) == String(MQTT_TOPIC_STRIP_HEX)) {
    //to RGB
    payload[len] = 0;
    unsigned long int rgb=strtol((char *)&payload[1], NULL, 16);
    byte r=(byte)((rgb >> 16) & 0xFF);
    byte g=(byte)((rgb >> 8) & 0xFF);
    byte b=(byte) (rgb & 0xFF);
    setColorAllPixels(r, g, b);
  } else {
    // i don't know ...
  }
}

/****************************************
 * OTA Updates
 ****************************************/
void reconnectToOta()
{
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

  otaReconnectTimer = xTimerCreate("otaTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(reconnectToOta));

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
  memset(pixels, 0, LED_STRIP_LED_COUNT * sizeof(pixel_t));
  setColorAllPixels(LED_STRIP_INITIAL_COLOR_R, LED_STRIP_INITIAL_COLOR_G, LED_STRIP_INITIAL_COLOR_B);
  setAllPixelsBrightness(LED_STRIP_INITIAL_BRIGHTNESS);
  setAllPixelsOnOff(LED_STRIP_INITIAL_ONOFF);
}

void setColorAllPixels(byte red, byte green, byte blue) 
{
  Serial.print("setColorAllPixels: ");
  Serial.print(red);
  Serial.print(", ");
  Serial.print(green);
  Serial.print(", ");
  Serial.println(blue);

  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    float ratio = (float)pixels[i].brightness / 255.0;
    pixels[i].r = red;
    pixels[i].g = green;
    pixels[i].b = blue;
    if (pixels[i].enabled)
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
    else
      led_strip.setPixelColor(i, led_strip.Color(0, 0, 0));    
  }

  led_strip.show();
  delay(50);
}

void setColorAllPixelsRed(byte red) 
{
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    float ratio = (float)pixels[i].brightness / 255.0;
    pixels[i].r = red;
    if (pixels[1].enabled)
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
  }

  led_strip.show();
}

void setColorAllPixelsGreen(byte green) 
{
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    float ratio = (float)pixels[i].brightness / 255.0;
    pixels[i].g = green;
    if (pixels[1].enabled)
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
  }

  led_strip.show();
}

void setColorAllPixelsBlue(byte blue) 
{
  led_strip.clear();

  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    float ratio = (float)pixels[i].brightness / 255.0;
    pixels[i].b = blue;
    if (pixels[1].enabled)
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
  }

  led_strip.show();
}

void setAllPixelsBrightness(byte brightness)
{
  float ratio = (float)brightness / 255.0;
  
  Serial.print("setBrightness: ");
  Serial.println(brightness);

  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    pixels[i].brightness = brightness;
    if (pixels[1].enabled)
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
  }

  led_strip.show();
  delay(50);
}

void setAllPixelsOnOff(bool enable)
{
  char msg[10];

  Serial.print("setAllPixelsOnOff: ");
  if (enable) Serial.println("Enable");
  else Serial.println("Disable");
  
  for (byte i=0; i < LED_STRIP_LED_COUNT; i++) {
    pixels[i].enabled = enable;
    if (enable) {
      Serial.print(i); Serial.print(": ");
      Serial.print(pixels[i].r); Serial.print(", ");
      Serial.print(pixels[i].g); Serial.print(", ");
      Serial.println(pixels[i].b);
      float ratio = (float)pixels[i].brightness / 255.0;
      led_strip.setPixelColor(i, led_strip.Color((byte)((float)pixels[i].r * ratio), 
                                                 (byte)((float)pixels[i].g * ratio),
                                                 (byte)((float)pixels[i].b * ratio)));
    } else
      led_strip.setPixelColor(i, led_strip.Color(0, 0, 0));
  }
  led_strip.show();
  delay(50);

  snprintf (msg, 10, "%u", enable ? 1 : 0);
  mqtt_publish(MQTT_TOPIC_STRIP_ON_OFF_FEEDBACK, msg);
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
