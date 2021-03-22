/**
 * @file main.cpp
 * @brief Main source file for Wall-E camera firmware
 * @date 2021-03-22
 * @author DIY Embedded Systems (diy.embeddedsytems@gmail.com)
 * 
 */

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    Includes
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#include "version.h"            /* Versioning macros, generated in the build_version.py script */
#include <Arduino.h>            /* The Arduino framework */
/* ~~~~~ Generic libraries for ESP32 */
// #include <GDBStub.h>         /* Enable debugging */
#include <Esp.h>                /* Get infos about ESP reset, stack/heap, etc. */
#include "soc/soc.h"          
#include "soc/rtc_cntl_reg.h"  /* Disable brownout detection */

#include <WiFi.h>               /* WiFi functions */
#include <AsyncTCP.h>           /* Asynchronous web and websocket servers  */
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

#include <FS.h>                 /* Generic filesystem functions */
#include <SPIFFS.h>             /* SPI flash filesystem */

#include <Update.h>             /* firmware update */

#include <esp32cam.h>           /* Camera interface */

/* ~~~~~  Project-local dependencies */
#include <credentials.h>        /* Wifi access point credentials, IP configuration macros */
#include "trigger.h"            /* Utility for periodic tasks */

#include <ArduinoLogger.h>      /* Logging library */
#include <SerialLogger.h>       /* Log to Serial */

#include "servers.h"

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    Function declarations / prototypes
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void setup();
void loop();
void blink(uint32_t high_ms, uint32_t low_ms);

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    Global variables & object instances
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
SerialLogger logger = SerialLogger::getDefault(); // log to Serial only (note: seems UDPLogger doesn't work here)

const int pin_red_led = 33;   // red LED on the back on pin nb. 33
const int pin_flash_led = 4;  // flash LED on pin nb. 4


/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    Function implementation
   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */


/* ~~~~~ Platform initialization ~~~~~ */

/**
 * @brief Set up software runtime
 */
void setup() {
  pinMode(pin_red_led, OUTPUT);
  digitalWrite(pin_red_led, LOW); // active low

  Serial.begin(115200);
  logger.setContext("ESP32-CAM");
  logger.info("\n\n\n");
  logger.info("Application " __FILE__ " compiled " __DATE__ " at " __TIME__ );

  // Setup SPIFFS filesystem
  if(!SPIFFS.begin()){ 
    logger.warn("Could not mount SPIFFS");
  } else {
    logger.info("SPIFFS total %d kB", SPIFFS.totalBytes()/1024);
    logger.info("SPIFFS content: %s", jsonFileSystem().c_str());
  }

  // Setup camera
  esp32cam::Config cfg;
  cfg.setPins(esp32cam::pins::AiThinker);
  cfg.setResolution(esp32cam::Resolution::find(320, 200));
  cfg.setBufferCount(1);
  cfg.setJpeg(80);
  if (esp32cam::Camera.begin(cfg)) {
    logger.info("Camera is up");
  } else {
    logger.warn("Camera is down");
  }

  // Setup Wifi
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ESP32-Cam");

  // Mind the arguments order! Might not be the same as in ESP8266...
  WiFi.config(
    IPAddress(192,168,1,201),     /* my IP address */
    IPAddress(192,168,1,178),     /* gateway */
    IPAddress(255,255,255,0),     /* subnet */    
    IPAddress(8,8,8,8));          /* DNS */
  WiFi.disconnect();
 
  logger.info("Connecting to %s", WIFI_STASSID);
  WiFi.begin(WIFI_STASSID, WIFI_STAPSK);
  while (WiFi.status() != WL_CONNECTED) {
    static uint32_t text_ms = 0;
    if (periodicTrigger(&text_ms, 1000)) {
      logger.info(".");
    }
    blink(450, 50);
    delay(10);
  }
  logger.info("Wifi connected to %s, I am %s", WIFI_STASSID, WiFi.localIP().toString().c_str());

  // Turn-off brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Setup web and websocket servers
  serversSetup();

  httpServer.on("/picture.jpg", HTTP_GET, [](AsyncWebServerRequest *request) {
      esp32cam::Resolution hiRes = esp32cam::Resolution::find(800, 600);
      if (!esp32cam::Camera.changeResolution(hiRes)) {
        logger.warn("Could not set resolution");
      }
      std::unique_ptr<esp32cam::Frame> frame = esp32cam::capture();
      if (frame) {
        logger.info("Captured image: %u x %u, %s", 
            frame->getWidth(), frame->getHeight(), readableSize(frame->size()));

        AsyncWebServerResponse *response = request->beginChunkedResponse("image/jpeg", [&](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
          if (frame->size() - index > maxLen) {
            maxLen = frame->size() - index;
          }
          memcpy(buf, &frame->data()[index], maxLen);
          return maxLen;
        });

        request->send(response);
        //request->send(200, "image/jpeg", (const uint8_t *)frame->data(), (size_t)frame->size());
        logger.info("Image sent");
      } else {
        logger.warn("Camera capture failed!");
        request->send(503, "text/plain", "capture failed");
      } 
  });
}

/**
 * @brief Main loop
 */
void loop() {
  blink(50, 950);

  static uint32_t next_report_ms = 0;
  if (periodicTrigger(&next_report_ms, 10000)) {
    logger.info("Still alive at %ums", millis());
  }

  if (wsServer.count() > 0) {
    static uint32_t next_frame_ms = 0;
    // We have a websocket client connected: send him video pictures
    if (periodicTrigger(&next_frame_ms, 100)) {
      uint32_t t0_us = micros();
      esp32cam::Resolution loRes = esp32cam::Resolution::find(640,480);
      esp32cam::Camera.changeResolution(loRes);
      std::unique_ptr<esp32cam::Frame> frame = esp32cam::capture();
      uint32_t t1_us = micros();

      if (frame && ESP.getFreeHeap() > 60000) {
        // Ugly workaround: 
        // it seems that AsyncWebSocket::binaryAll crashes at `new AsyncWebSocketMessageBuffer()`, when heap falls below 60kB...
        wsServer.binaryAll(frame->data(), frame->size());
        uint32_t t2_us = micros();
        logger.info("capture %u us, binaryAll %u us, picture %u B, heap %u B, PSRAM %u B", 
            t1_us  - t0_us, t2_us - t1_us,
            frame->size(), ESP.getFreeHeap(), ESP.getFreePsram());
      } else {
        logger.warn("wsStream: capture failed");
      }
    }
  }
}


/**
 * @brief blink the RED LED with configurable pattern
 * Note: you should call this function frequently, at least several times per high or low porch, to actually see the right blinking pattern
 * @param high_ms duration (in milliseconds) of the high porch 
 * @param low_ms duration (in milliseconds) of the low porch
 */
void blink(uint32_t high_ms, uint32_t low_ms) {
  static uint32_t next_blink_ms = 0;
  static bool state = false;
  uint32_t now_ms = millis();
  while ((int32_t)(now_ms - next_blink_ms) > 0) {
    state = !state;
    digitalWrite(pin_red_led, state ? LOW : HIGH);
    next_blink_ms += state ? high_ms : low_ms;
  }
}