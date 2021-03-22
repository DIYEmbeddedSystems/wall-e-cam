
/**
 * @file webServer.h
 * @brief Wall-E camera firmware: web servers module
 * @date 2021-03-22
 * @author DIY Embedded Systems (diy.embeddedsytems@gmail.com)
 * 
 */
#ifndef SERVERS_H
#define SERVERS_H

#include <Arduino.h>
#include <WiFi.h>               /* WiFi functions */
#include <AsyncTCP.h>           /* Asynchronous web and websocket servers  */
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

extern AsyncWebServer httpServer;
extern AsyncWebSocket wsServer;
extern int lastClient;

void serversSetup();
void serversLoop();

void websocketEventHandler(AsyncWebSocket * server, AsyncWebSocketClient * client, 
    AwsEventType eventType, void * arg, uint8_t *payload, size_t len);

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

String jsonVersion();
String jsonFileSystem();
String jsonDirectory(File dir);
const char *readableSize(int size);

#endif