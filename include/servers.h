
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
#include <SPIFFS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>


extern int lastClient;

extern WebServer httpServer;
extern WebSocketsServer wsServer;

void serversSetup();
void serversLoop();

void handleUpload();

void websocketEventHandler(uint8_t num, WStype_t eventType, uint8_t *payload, size_t len);


String jsonVersion();
String jsonFileSystem();
String jsonDirectory(File dir);
const char *readableSize(int size);
String getContentType(String filename);

#endif