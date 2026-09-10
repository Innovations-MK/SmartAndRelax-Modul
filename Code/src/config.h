#include <Arduino.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include "FW_VERSION.h"


#if __has_include("sar_private_config.h")
#include "sar_private_config.h"
#endif

#ifndef SAR_PUBLIC_CLOUD_HOST
#define SAR_PUBLIC_CLOUD_HOST ""
#endif
#ifndef SAR_PUBLIC_CLOUD_PORT
#define SAR_PUBLIC_CLOUD_PORT 8884
#endif
#ifndef SAR_PUBLIC_TOPIC_PREFIX
#define SAR_PUBLIC_TOPIC_PREFIX "devices/"
#endif
#ifndef SAR_PUBLIC_MIGRATION_API_URL
#define SAR_PUBLIC_MIGRATION_API_URL ""
#endif
#ifndef SAR_PUBLIC_PRESENCE_HOST
#define SAR_PUBLIC_PRESENCE_HOST ""
#endif
#ifndef SAR_PUBLIC_UPDATE_HOST
#define SAR_PUBLIC_UPDATE_HOST ""
#endif
#ifndef SAR_PUBLIC_UPDATE_INFO_URL
#define SAR_PUBLIC_UPDATE_INFO_URL ""
#endif

#define DEVICE_NAME "layzspa"

#define HA_PREFIX "homeassistant"
#define PROM_NAMESPACE "layzspa"


const bool hidePasswords = true;

const char *netHostname = DEVICE_NAME;

bool notify = false;

int notification_time = 32;


bool enableWebAuth = false;

String authUsername = "username";

String authPassword = "password";


const char *OTAName = DEVICE_NAME;

const char *OTAPassword = "esp8266";


bool showSectionTemperature = true;

bool showSectionDisplay = true;

bool showSectionControl = true;

bool showSectionButtons = true;

bool showSectionTimer = true;

bool showSectionTotals = true;

bool useControlSelector = false;


const char *wmApName = "Lay-Z-Spa Module";

const char *wmApPassword = "layzspam0dule";


        


        
        
        
        
        
        

        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        


bool useMqtt = false;


IPAddress mqttCustomIpAddress(192,168,0,20);
int      mqttCustomPort = 1883;
String   mqttCustomUsername = "username";
String   mqttCustomPassword = "password";
String   mqttCustomClientId = DEVICE_NAME;
String   mqttCustomBaseTopic = DEVICE_NAME;


IPAddress mqttIpAddress(192,168,0,20);
int mqttPort = 1883;
String mqttUsername = "username";
String mqttPassword = "password";
String mqttClientId = DEVICE_NAME;
String mqttBaseTopic = DEVICE_NAME;

int mqttTelemetryInterval = 600;


bool mqttCloudMode = (SAR_PUBLIC_CLOUD_HOST[0] != '\0');
static const bool SAR_CLOUD_V2_ALWAYS_ON = (SAR_PUBLIC_CLOUD_HOST[0] != '\0');

String mqttPairingCode = "";
String mqttPairingSentHash = "";


static const char* SAR_CLOUD_HOST = SAR_PUBLIC_CLOUD_HOST;
static const uint16_t SAR_CLOUD_PORT = SAR_PUBLIC_CLOUD_PORT;
static const char* SAR_TOPIC_PREFIX = SAR_PUBLIC_TOPIC_PREFIX;
