/**
 * @file webServer.cpp
 * @brief Wall-E camera firmware: web servers module
 * @date 2021-03-22
 * @author DIY Embedded Systems (diy.embeddedsytems@gmail.com)
 * 
 * 
 * Notes:
 * - I could not have the file upload work with files > ~700kiB (due to write delay and timeout?); therefore no FW update this way
 */
#include "servers.h"
#include "version.h"

#include <Arduino.h>
#include <Esp.h>                /* Get infos about ESP reset, stack/heap, etc. */
#include <WiFi.h>               /* WiFi functions */
#include <AsyncTCP.h>           /* Asynchronous web and websocket servers  */
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

#include <SPIFFS.h>
#include <SerialLogger.h>


extern SerialLogger logger;     /* logger declared in main.cpp */
int lastClient = -1;

AsyncWebServer httpServer(80);  /* HTTP server instance */
AsyncWebSocket wsServer("/ws");  /* Websocket server instance */


/**
 * @brief Configure the web server
 */
void serversSetup() {
  /* Serve files stored in SPI flash filesystem */
  httpServer.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  
  /* Server APIs */
  httpServer.on("/list", HTTP_GET, [](AsyncWebServerRequest *request) 
    {
      request->send(200, "application/json", jsonFileSystem());
    });  
 
  httpServer.on("/version", HTTP_GET, [](AsyncWebServerRequest *request) 
    {
      request->send(200, "application/json", jsonVersion());
    });

  httpServer.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request)
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "{\"heap\":%u}\n", ESP.getFreeHeap());
      request->send(200, "application/json", msg);
    });

  httpServer.on("/delete", HTTP_ANY, [](AsyncWebServerRequest *request) 
    {
      if(request->hasParam("path")) {
        String path = request->getParam("path")->value();
        if (!path.startsWith("/")) {
          path = "/" + path;
        }
        if (SPIFFS.exists(path)) {
          logger.warn("Deleting file `%s`", path.c_str());
          SPIFFS.remove(path);
          request->send(200, "text/plain", "ok");
        } else {
          logger.warn("File `%s` not found", path.c_str());
          request->send(200, "text/plain", "Not found");
        }
      } else{
        logger.warn("Delete: wrong path");
        request->send(200, "text/plain", "Usage: delete?path=/file.txt");
      }
    });

  httpServer.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
      logger.info("Upload endpoint. Sending 200 OK");
      request->send(200);
    },
    handleUpload);

  httpServer.onNotFound([](AsyncWebServerRequest *request) {
      logger.info("http: %s %s%s not found", 
          (request->method() == HTTP_GET) ? "GET" : (request->method() == HTTP_POST) ? "POST" : "METHOD?",
          request->host().c_str(),
          request->url().c_str());
      request->send(404, "text/plain", "404: Not Found\n");
    });


  /* Setup websocket server */
  wsServer.onEvent(websocketEventHandler);
  httpServer.addHandler(&wsServer);

  /* Start */
  httpServer.begin();
}



void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    // open the file on first call and store the file handle in the request object
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    request->_tempFile = SPIFFS.open("/" + filename, "w");
    logger.info("Opening file %s", filename);
  }

  if (len) {
    // stream the incoming chunk to the opened file
    if (request->_tempFile) {
      request->_tempFile.write(data, len);
    } else {
      logger.error("request->_tempFile closed");
    }
    logger.info("%s: %s", filename, readableSize(index+len));
  }

  if (final) {
    if (request->_tempFile) {
      // close the file handle as the upload is now done
      request->_tempFile.close();
      logger.info("Upload finished!");
    } else {
      logger.error("request->_tempFile closed");
    }
    request->send(200, "text/plain", "Upload finished");
  }
}


/**
 * @brief Do some housekeeping 
 * Note: requests and message handling is done in another context
 */
void serversLoop() {
  static uint32_t next_cleanup_ms = 0;
  if ((int32_t)(millis() - next_cleanup_ms) > 0) {
    wsServer.cleanupClients();
    next_cleanup_ms += 1000;
  }
}

/**
 * @brief User callback that handles generic websocket events
 * @param server
 * @param client
 * @param eventType
 * @param arg
 * @param payload
 * @param len
 */
void websocketEventHandler(AsyncWebSocket * server, AsyncWebSocketClient * client, 
    AwsEventType eventType, void * arg, uint8_t *payload, size_t len) {
  
  AwsFrameInfo *frameInfo;

  switch (eventType) {
  case WS_EVT_CONNECT:
    logger.info("[WS] New client #%u from %s", client->id(), client->remoteIP().toString().c_str());
    lastClient = client->id();
    break;

  case WS_EVT_DISCONNECT:
    logger.info("[WS] Client #%u has left", client->id());
    if (lastClient == client->id()) {
      lastClient = -1;
    }
    break;

  case WS_EVT_PONG:
    logger.info("[WS] Received pong");
    break;

  case WS_EVT_DATA:
    lastClient = client->id();
    frameInfo = (AwsFrameInfo *)arg;
    // is this frame a full unfragmented websocket TEXT frame?
    if (!frameInfo->final || frameInfo->index != 0 || frameInfo->len != len) {
      logger.warn("[WS] frame fragmentation not supported");
    } else if (frameInfo->opcode != WS_TEXT) {
      logger.info("[WS] Binary frame");
    } else {
      logger.info("[WS] Text frame `%.*s`", len, payload);
    }
    break;

  case WS_EVT_ERROR:
    logger.warn("[WS] client #%u error #%u `%.*s`", client->id(), *(uint16_t*)arg, len, (char *)payload);
    break;

  default:
    logger.error("[WS] Event type %u not supported", eventType);
    break;
  }
}

/**
 * @brief Describe filesystem in JSON format
 */
String jsonFileSystem() {
  String s = "";
  s += "{\"totalSize\":\"";
  s += readableSize(SPIFFS.totalBytes());
  s += "\", \"usedSize\":\"";
  s += readableSize(SPIFFS.usedBytes());
  s += "\", \"files\":[";
  s += jsonDirectory(SPIFFS.open("/"));
  s += "]}\n";
  logger.info("filesJSON: %s", s.c_str());
  return s;
}

/**
 * @brief Describe build version in JSON format
 */
String jsonVersion()
{
  String s = "";
  s += "{\"build-date\":\"" BUILD_DATE "\"";
  s += ",\"build-nb\":\"" + String(BUILD_NUMBER) + "\"";
  s += ",\"repository\":\"" GIT_REPO_URL "\"";
  s += ",\"hash\":\"" GIT_DESCRIPTION "\"";
  s += ",\"branch\":\"" GIT_BRANCH "\"";
  s += "}\n";
  return s;
}

/**
 * @brief Describe filesystem directory in JSON format (warning: recursive function)
 */
String jsonDirectory(File dir) {
  // {"dirname" : "/", "content": [{"filename": "FILE.TXT", "size": 123}, {"dirname": "SUBFOLDER", "content": [...]}, ...]}
  String s = "{\"directory\":\"";
  s += dir.name();
  s += "\", \"content\":[";
  File f;
  int count = 0;
  do {
    f = dir.openNextFile();
    if (count) {
      s += ",";
    }
    if (f.isDirectory()) {
      s += jsonDirectory(f);
    } else if (f) {
      s += "{\"filename\":\"";
      s += f.name();
      s += "\", \"size\":\"";
      s += String(readableSize(f.size()));
      s += "\"}";
    }
    ++count;
  } while (f);
  s += "]}";
  return s;
}

/**
 * @brief Human-friendly description of file size
 * @param size in bytes
 * @return pointer to (static!) string, e.g. "3.4kiB" for 35651584 bytes
 */
const char *readableSize(int size)
{
  static char buffer[128];
  if (size > 1024*1024) {
    snprintf(buffer, sizeof(buffer)-1, "%.1fMiB", size*1.0/1024/1024);
  } else if (size > 1024) {
    snprintf(buffer, sizeof(buffer)-1, "%.1fkiB", size*1.0/1024);
  } else {
    snprintf(buffer, sizeof(buffer)-1, "%uB", size);
  }
  return buffer;
}