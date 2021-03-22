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
#include <WebServer.h>

#include <SPIFFS.h>
#include <SerialLogger.h>


extern SerialLogger logger;     /* logger declared in main.cpp */
int lastClient = -1;

WebServer httpServer(80);
WebSocketsServer wsServer = WebSocketsServer(81);

static File uploadFile;


/**
 * @brief Configure the web server
 */
void serversSetup() {
  /* Serve files stored in SPI flash filesystem */
//  httpServer.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  
  /* Server APIs */
  httpServer.on("/", HTTP_GET, []() {
      httpServer.sendHeader("Connection", "close");
      httpServer.send(200, "text/html", "hello");
    });

  httpServer.on("/list", HTTP_GET, []() {
      httpServer.send(200, "text/json", jsonFileSystem());
    });
 
  httpServer.on("/version", HTTP_GET, []() {
      httpServer.send(200, "application/json", jsonVersion());
    });

  httpServer.on("/heap", HTTP_GET, []() {
      char msg[256];
      snprintf(msg, sizeof(msg), "{\"heap\":%u}\n", ESP.getFreeHeap());
      httpServer.send(200, "application/json", msg);
    });

  httpServer.on("/delete", HTTP_ANY, []() {
      if (httpServer.hasArg("path")) {
        String path = httpServer.arg("path");
        if (!path.startsWith("/")) {
          path = "/" + path;
        }
        if (SPIFFS.exists(path)) {
          logger.warn("Deleting file `%s`", path.c_str());
          SPIFFS.remove(path);
          httpServer.send(200, "text/plain", "ok");
        } else {
          logger.warn("File `%s` not found", path.c_str());
          httpServer.send(200, "text/plain", "Not found");
        }
      } else{
        logger.warn("Delete: wrong path");
        httpServer.send(200, "text/plain", "Usage: delete?path=/file.txt");
      }
    });

  httpServer.on("/upload", HTTP_POST, []() {
        logger.info("Upload finished?");
        httpServer.send(200, "text/plain", "OK...");
      }, handleUpload);

  httpServer.onNotFound([]() {
      String uri = httpServer.uri();
      if (uri.endsWith("/")) {
        uri += "index.html";
      }
      File file = SPIFFS.open(uri.c_str());
      if (file) {
        httpServer.streamFile(file, getContentType(uri));
      } else {
        httpServer.send(404, "text/plain", "Not found");
      }
    });

  /* Start */
  httpServer.begin();

  /* Setup websocket server */
  wsServer.onEvent(websocketEventHandler);
  wsServer.begin();
}


void handleUpload() {
  HTTPUpload& upload = httpServer.upload();
  String filename = upload.filename;
  static uint32_t t_start = millis();

  if (upload.status == UPLOAD_FILE_START) {
    logger.info("Uploading %s", filename.c_str());
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    if (SPIFFS.exists((char *)filename.c_str())) {
      SPIFFS.remove((char *)filename.c_str());
    }
    uploadFile = SPIFFS.open(filename.c_str(), FILE_WRITE);
    if (uploadFile) {
      logger.info("upload file %s open", filename.c_str());
    } else {
      logger.warn("upload file %s could not open", filename.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
    logger.info("Upload: %s (avg %u kB/s)", 
        readableSize(upload.totalSize), upload.totalSize / (millis() - t_start));
  } else if (upload.status == UPLOAD_FILE_END) {
    logger.info("Upload end");
    if(uploadFile) {
      logger.info("Upload successful: %s (%s)", filename.c_str(), readableSize(uploadFile.size()));
      uploadFile.close();
    } else {
      logger.warn("Where's our file?");
    }
  }
}

/**
 * @brief Do some housekeeping 
 * Note: requests and message handling is done in another context
 */
void serversLoop() {
  httpServer.handleClient();
  wsServer.loop();
}

/**
 * @brief User callback that handles generic websocket events
  */
void websocketEventHandler(uint8_t num, WStype_t eventType, uint8_t *payload, size_t len) {

  switch (eventType) {
  case WStype_CONNECTED:
    logger.info("[WS] New client #%u from %s", num, wsServer.remoteIP(num).toString().c_str());
    lastClient = num;
    break;

  case WStype_DISCONNECTED:
    logger.info("[WS] Client #%u has left", num);
    if (lastClient == num) {
      lastClient = -1;
    }
    break;

  case WStype_TEXT:
    lastClient = num;
    logger.info("[WS] Text frame `%.*s`", len, payload);
    break;

  case WStype_BIN:
    lastClient = num;
    logger.info("[WS] Binary frame (len %u)", len);
    break;

  case WStype_ERROR:
    logger.warn("[WS] client #%u ERROR", num);
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

/**
 * @brief Get the content-type field from file extension
 */
String getContentType(String filename){
  if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}