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
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <FS.h>                 /* Generic filesystem functions */
#include <SPIFFS.h>             /* SPI flash filesystem */

#include <Update.h>             /* firmware update */

#include <esp32cam.h>           /* Camera interface */

#include <freertos/task.h>
#include <esp_task_wdt.h>

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
  logger.info("Version: %s", jsonVersion().c_str());

  if (psramFound()) {
    logger.info("This chip has PSRAM %u B (%u free)", ESP.getPsramSize(), ESP.getFreePsram());
  } else {
    logger.info("This chip has no PSRAM");
  }

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
  cfg.setResolution(esp32cam::Resolution::find(800, 600));
  cfg.setBufferCount(2);
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

  httpServer.on("/picture.jpg", HTTP_GET, []() {
    std::unique_ptr<esp32cam::Frame> frame = esp32cam::capture();
    if (frame) {
      logger.info("Captured image: %u x %u, %s", 
          frame->getWidth(), frame->getHeight(), readableSize(frame->size()));
      
      httpServer.setContentLength(frame->size());
      httpServer.send(200, "image/jpeg");

      WiFiClient client = httpServer.client();
      if (frame->writeTo(client)) {
        logger.info("Image sent");
      } else {
        logger.info("Image partially sent");
      }

      
    } else {
      logger.warn("Camera capture failed!");
      httpServer.send(503, "text/plain", "capture failed");
    } 
  });

  httpServer.on("/resolution", HTTP_GET, []() {
    if (httpServer.hasArg("width")) {
      int width = httpServer.arg("width").toInt();
      esp32cam::Resolution res = esp32cam::Resolution::find(width, width / 2);
      if (esp32cam::Camera.changeResolution(res)) {
        logger.info("Changed resolution to %u x %u", res.getWidth(), res.getHeight());
        httpServer.send(200, "Resolution changed");
      } else {
        logger.warn("Could not set resolution");
        httpServer.send(200, "Could not set resolution");
      }
    }
  });

 }


/**
 * @brief Main loop
 */
void loop() {
  serversLoop();

  blink(50, 950);

  static uint32_t next_report_ms = 0;
  if (periodicTrigger(&next_report_ms, 10000)) {
    logger.info("Core %u, free heap %s (max %s), free PSRAM %s (max %s), %u tasks", 
        xPortGetCoreID(),
        readableSize(ESP.getFreeHeap()).c_str(),
        readableSize(ESP.getMaxAllocHeap()).c_str(),
        readableSize(ESP.getFreePsram()).c_str(), 
        readableSize(ESP.getMaxAllocPsram()).c_str(), 
        uxTaskGetNumberOfTasks()
        );
  }

  if (wsServer.connectedClients() > 0) {
    static uint32_t next_frame_ms = 0;
    // We have a websocket client connected: send pictures
    
    if (periodicTrigger(&next_frame_ms, 1000/24)) { // Don't try more than 24 frames per second
      uint32_t t0_us = micros();
      std::unique_ptr<esp32cam::Frame> frame = esp32cam::capture();
      uint32_t t1_us = micros();

      if (frame) {
        wsServer.broadcastBIN(frame->data(), frame->size());
        uint32_t t2_us = micros();
        logger.info("%u clients, capture %u ms, websocket %u ms, picture %s, heap %s, PSRAM %s", 
            wsServer.connectedClients(),
            (t1_us  - t0_us)/1000, 
            (t2_us - t1_us)/1000,
            readableSize(frame->size()).c_str(), 
            readableSize(ESP.getFreeHeap()).c_str(), 
            readableSize(ESP.getFreePsram()).c_str());
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