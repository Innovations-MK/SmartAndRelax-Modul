#pragma once
#include <Arduino.h>
#include <ArduinoOTA.h>
                         

#ifdef ESP8266

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include <time.h>

#else

#include <WebServer.h>
#include <WiFi.h>

#endif

#include <OneWire.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <PubSubClient.h> // ** Requires library 2.8.0 or higher ** https://github.com/knolleary/pubsubclient
#include <Ticker.h>
#include <WebSocketsServer.h>
                               
#include <WiFiManager.h>
#define ESP_WiFiManager WiFiManager
#include <umm_malloc/umm_heap_select.h>

#include "bwc.h"
#include "config.h"
#include "util.h"

  
#if BWC_DEBUGGING == 1
    #define BWC_LOG_P(pstr_string, ...) Serial.printf_P(pstr_string, __VA_ARGS__)
    #define BWC_LOG(s, ...) Serial.printf(s, __VA_ARGS__)
#else
    #define BWC_LOG_P(s, ...) 
#endif

BWC *bwc = nullptr;

                                                                           
Ticker bootlogTimer;
Ticker periodicTimer;
Ticker startComplete_ticker;
Ticker ntpCheck_ticker;
Ticker checkWifi_ticker;

                                                            
bool periodicTimerFlag = false;
bool checkNTP_flag = false;
bool CheckWiFi_flag = false;
       
int periodicTimerInterval = 60;
                                                           
bool wifiConnected = false;
sWifi_info wifi_info;

                                                         
                      

                                                 
#if defined(ESP8266)
ESP8266WebServer *server = nullptr;
#elif defined(ESP32)
WebServer server(80);
#endif
                                                             
File fsUploadFile;

                                                 
WebSocketsServer *webSocket = nullptr;
       
Ticker updateWSTimer;
       
bool sendWSFlag = false;

                                                   
                                         
Client *aWifiClient = nullptr;
#if defined(ESP8266)
#include <WiFiClientSecureBearSSL.h>
extern BearSSL::WiFiClientSecureCtx *tlsClient;
extern BearSSL::X509List *tlsCa;
#endif



                    
PubSubClient *mqttClient = nullptr;
       
bool checkMqttConnection = false;
                                                                                                           
int mqtt_connect_count;
       
String prevButtonName = "";
       
bool prevunit = 1;
       
Ticker updateMqttTimer;
       
bool sendMQTTFlag = false;
bool enableMqtt = false;
bool send_mqtt_cfg_needed = false;

                           
bool runonce = true;

void sendWS();
void getOtherInfo(String &rtn);
void sendMQTT();
void sendMQTTConfig();
void startWiFi();
void checkWiFi_ISR();
void checkWiFi();
void startWiFiConfigPortal();
void checkNTP_ISR();
void checkNTP();
void startNTP();
void startOTA();
void stopall();
void pause_all(bool action);
void pause_cloud_tasks_only(bool action);
void startWebSocket();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t len);
void startHttpServer();
                            
void handleGetHardware();
void handleSetHardware();
void handleHWtest();
void handleNotFound();
String getContentType(const String& filename);
bool handleFileRead(String path);
bool checkHttpPost(HTTPMethod method);
void handleGetConfig();
void handleSetConfig();
void handleGetCommandQueue();
void handleAddCommand();
void handleEditCommand();
void handleDelCommand();
void handle_cmdq_file();
void copyFile(String source, String dest);
void loadWebConfig();
void saveWebConfig();
void handleGetWebConfig();
void handleSetWebConfig();
void loadWifi();
void saveWifi();
void handleGetWifi();
void handleSetWifi();
void handleResetWifi();
void resetWiFi();
void loadMqtt();
void saveMqtt();
void handleGetMqtt();
void handleSetMqtt();
void handleDir();
void handleFileUpload();
void handleFileRemove();
void handleRestart();
void updateStart();
void updateEnd();
void udpateProgress(int cur, int total);
void updateError(int err);
void startMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttConnect();
time_t getBootTime();
void handleESPInfo();
void setTemperatureFromSensor();

void setupHA();
void handlePrometheusMetrics();

           
void write_mem_stats_to_file();
void preparefortest();
void handleInputs();