#include "main.h"
#include "mqtt_ca.h"
#include "mqtt_cloud_ca_root_ye.h"
#include "cloud_v2_credentials.h"
void sarRunPendingOnlineUpdate();
static void sarHandleSerialProvisioning();
static void sarSerialProvisioningBootWindow(uint32_t windowMs);
static void sarCloudV2MigrationLoadState();
static void sarCloudV2MigrationClearState();
static void sarCloudV2MigrationTick();
static void sarPrepareFreshV2PairingAfterMigration();

                                                                
static uint32_t mqtt_next_telemetry_ms = 0;
static bool mqtt_telemetry_enabled = false;

                                                                            
                                                                             
static uint32_t cloud_next_mqtt_try_ms = 0;

static void writeRestartMarker(const String& reason);
static void requestRestart(const char* reason);
#if defined(ESP8266)
static void cloudTlsStopBounded(bool gracefulMqttDisconnect, const char* reason);
#endif
bool setupHAStage(uint8_t stage);
static void publishStatusRetained(const char* status);

                                                       
                                                                          
                                                                              
                                                                              
static void cloudV2ResponseTick();
static bool cloud_v2_publish_telemetry_pending = false;
static bool cloud_v2_publish_times_pending = false;
static bool cloud_v2_publish_config_pending = false;
static bool cloud_v2_publish_queue_pending = false;
static bool cloud_v2_publish_smartschedule_pending = false;

struct CloudV2AckPending
{
    bool pending = false;
    bool ok = false;
    char op[24] = {0};
    char rid[40] = {0};
    char detail[72] = {0};
};
static CloudV2AckPending cloud_v2_ack;

static inline void mqttServiceTick()
{
    if (!mqttClient) return;
    mqttClient->loop();
    yield();
    delay(0);
}

static bool mqttPublishChecked(const String& topic, const String& payload, bool retain)
{
    if (!mqttClient || !mqttClient->connected()) return false;
    if (!mqttClient->publish(topic.c_str(), payload.c_str(), retain)) {
        mqttServiceTick();
        return false;
    }
    mqttServiceTick();
    return true;
}

static bool mqttPublishChecked(const String& topic, const char* payload, bool retain)
{
    if (!mqttClient || !mqttClient->connected()) return false;
    if (!mqttClient->publish(topic.c_str(), payload, retain)) {
        mqttServiceTick();
        return false;
    }
    mqttServiceTick();
    return true;
}

                                                                                                
                                                                                                                               
                                                                                                                       
static uint32_t heap_guard_last_check_ms = 0;
static uint32_t low_heap_since_ms       = 0;
static uint32_t heap_guard_restarts     = 0;
static uint32_t min_heap_seen           = 0xFFFFFFFFUL;
static uint32_t min_maxblock_seen       = 0xFFFFFFFFUL;
                                                                              
                                                                             
                                                                              
                                                                          
static const uint32_t HEAP_GUARD_IDLE_MIN_HEAP  = 10000UL;
static const uint32_t HEAP_GUARD_IDLE_MIN_BLOCK = 5000UL;
static const uint32_t HEAP_GUARD_TLS_MIN_HEAP   = 6500UL;
static const uint32_t HEAP_GUARD_TLS_MIN_BLOCK  = 4000UL;
static bool     heap_guard_tls_runtime_mode     = false;
static uint32_t heap_guard_current_heap_limit   = HEAP_GUARD_IDLE_MIN_HEAP;
static uint32_t heap_guard_current_block_limit  = HEAP_GUARD_IDLE_MIN_BLOCK;

static void heapGuardTick(uint32_t now_ms)
{
                                        
    if (now_ms < 120000UL) return;

                                    
    if ((uint32_t)(now_ms - heap_guard_last_check_ms) < 5000UL) return;
    heap_guard_last_check_ms = now_ms;

    const uint32_t fh = ESP.getFreeHeap();
    const uint32_t mb = ESP.getMaxFreeBlockSize();

    if (fh < min_heap_seen)     min_heap_seen = fh;
    if (mb < min_maxblock_seen) min_maxblock_seen = mb;

                                                                             
                                                                           
                                                                                
    heap_guard_tls_runtime_mode = mqttCloudMode && mqttClient &&
                                  (mqttClient->state() == MQTT_CONNECTED);
    heap_guard_current_heap_limit = heap_guard_tls_runtime_mode
                                      ? HEAP_GUARD_TLS_MIN_HEAP
                                      : HEAP_GUARD_IDLE_MIN_HEAP;
    heap_guard_current_block_limit = heap_guard_tls_runtime_mode
                                       ? HEAP_GUARD_TLS_MIN_BLOCK
                                       : HEAP_GUARD_IDLE_MIN_BLOCK;
    const bool low = (fh < heap_guard_current_heap_limit) ||
                     (mb < heap_guard_current_block_limit);

    if (low) {
        if (low_heap_since_ms == 0) low_heap_since_ms = now_ms;

                                                                                       
        if ((uint32_t)(now_ms - low_heap_since_ms) > 20UL * 60UL * 1000UL) {
            heap_guard_restarts++;

            const String restartReason = String(F("LOW_HEAP fh=")) + fh + F(" mb=") + mb;

                                                                           
                                                                              
                                                                                 
#if defined(ESP8266)
            if (mqttCloudMode) cloudTlsStopBounded(false, "heap-guard");
            else if (aWifiClient) aWifiClient->stop();
#else
            if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
            if (aWifiClient) aWifiClient->stop();
#endif

            requestRestart(restartReason.c_str());
        }
    } else {
        low_heap_since_ms = 0;
    }
}

                                                                                  
static bool     custom_mqtt_kick = false;
static uint32_t custom_mqtt_kick_at_ms = 0;

                                                    
                                                                                    
                                                                                     
static bool     custom_ha_discovery_pending = false;
static bool     custom_ha_discovery_sent = false;
static uint8_t  custom_ha_discovery_stage = 0;
static uint32_t custom_ha_discovery_due_ms = 0;
static bool     mqtt_ha_discovery_active = false;                                                                 

static void resetCustomHaDiscoverySchedule()
{
    custom_ha_discovery_pending = false;
    custom_ha_discovery_sent = false;
    custom_ha_discovery_stage = 0;
    custom_ha_discovery_due_ms = 0;
}

static void armCustomHaDiscovery(uint32_t delayMs = 1500UL)
{
    custom_ha_discovery_pending = true;
    custom_ha_discovery_sent = false;
    custom_ha_discovery_stage = 0;
    custom_ha_discovery_due_ms = millis() + delayMs;
}

static void customHaDiscoveryTick()
{
    if (mqttCloudMode) return;
    if (!enableMqtt) return;
    if (!mqttClient || !mqttClient->connected()) return;
    if (!custom_ha_discovery_pending) return;
    if ((int32_t)(millis() - custom_ha_discovery_due_ms) < 0) return;

    switch (custom_ha_discovery_stage)
    {
        case 0:
        {
#ifdef ESP8266
            if (bwc) {
                if (!mqttPublishChecked(String(mqttBaseTopic) + F("/reboot_time"), bwc->reboot_time_str + 'Z', true)) {
                    custom_ha_discovery_pending = false;
                    custom_ha_discovery_sent = false;
                    custom_ha_discovery_stage = 0;
                    custom_ha_discovery_due_ms = millis() + 5000UL;
                    return;
                }
            }
            if (!mqttPublishChecked(String(mqttBaseTopic) + F("/reboot_reason"), ESP.getResetReason().c_str(), true)) {
                custom_ha_discovery_pending = false;
                custom_ha_discovery_sent = false;
                custom_ha_discovery_stage = 0;
                custom_ha_discovery_due_ms = millis() + 5000UL;
                return;
            }
#endif
            String buttonname;
            buttonname.reserve(32);
            if (bwc) bwc->getButtonName(buttonname);
            if (!mqttPublishChecked(String(mqttBaseTopic) + F("/button"), buttonname, true)) {
                custom_ha_discovery_pending = false;
                custom_ha_discovery_sent = false;
                custom_ha_discovery_stage = 0;
                custom_ha_discovery_due_ms = millis() + 5000UL;
                return;
            }
            prevButtonName = buttonname;
            mqttServiceTick();
            custom_ha_discovery_stage = 1;
            custom_ha_discovery_due_ms = millis() + 1200UL;
            Serial.println(F("HA discovery stage 0 done"));
            break;
        }

        case 1:
        {
            sendMQTT();
            mqttClient->loop();
                                                                                           
                                                        
            if (!mqttCloudMode) {
                uint32_t telemetryIntervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
                if (telemetryIntervalMs < 60000UL) telemetryIntervalMs = 60000UL;
                mqtt_telemetry_enabled = true;
                mqtt_next_telemetry_ms = millis() + telemetryIntervalMs;
            }
            custom_ha_discovery_stage = 2;
            custom_ha_discovery_due_ms = millis() + 1800UL;
            Serial.println(F("HA discovery stage 1 done"));
            break;
        }

        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        {
            const uint8_t haStage = custom_ha_discovery_stage - 2;
            if (haStage == 0) {
                Serial.println(F("MQTT Sending HA discovery (staged custom mode)"));
            }

            yield(); delay(0);
            mqtt_ha_discovery_active = true;
            bool haOk = setupHAStage(haStage);
            mqtt_ha_discovery_active = false;
            if (haOk) {
                yield(); delay(0);
                mqttClient->loop();

                if (custom_ha_discovery_stage < 9) {
                    custom_ha_discovery_stage++;
                    custom_ha_discovery_due_ms = millis() + 350UL;
                } else {
                    send_mqtt_cfg_needed = true;
                    custom_ha_discovery_pending = false;
                    custom_ha_discovery_sent = true;
                    custom_ha_discovery_stage = 0;
                    custom_ha_discovery_due_ms = 0;
                    Serial.println(F("HA discovery finished"));
                }
            } else {
                Serial.println(F("HA discovery stage failed"));
                custom_ha_discovery_pending = false;
                custom_ha_discovery_sent = false;
                custom_ha_discovery_stage = 0;
                custom_ha_discovery_due_ms = 0;
            }
            break;
        }

        default:
            resetCustomHaDiscoverySchedule();
            break;
    }
}

                                                                                           
static uint32_t presence_last_attempt_ms = 0;
static uint32_t presence_last_ok_ms      = 0;
static uint32_t presence_stall_count     = 0;
static uint32_t presence_force_poll_ms   = 0;
static uint32_t presence_last_stall_rearm_ms = 0;

                                                                        
static const uint32_t PRESENCE_NO_ATTEMPT_REARM_MS = 120000UL;                                                     
static const uint32_t PRESENCE_NO_OK_STALL_MS      = 900000UL;                                                      
static const uint32_t MQTT_BACKOFF_ON_STALL_MS     = 600000UL;                                                         
static const uint32_t PRESENCE_MQTT_FRESH_MAX_AGE_MS = 180000UL;                                                           



                                                      
static const char* SAR_BUILD_ID = "Phase15-Harden4-TargetGuardFix1-Unified6W-NoHold1-LockHold1-Beep1-ShortChain8884Test";
static String g_boot_diag;
static uint32_t g_boot_millis = 0;
static uint32_t g_boot_id = 0;
static uint32_t g_boot_heap_initial = 0;
static uint32_t g_boot_block_initial = 0;
static uint8_t  g_boot_frag_initial = 0;

                                                                      
                                                                            
                                                                            
                                                                   
static const bool SAR_LOCAL_WEBSOCKET_ENABLED = false;
static uint32_t http_file_serve_count = 0;
static uint32_t http_file_abort_count = 0;
static uint32_t http_file_write_stall_count = 0;
static uint32_t http_file_last_duration_ms = 0;
static uint32_t http_file_max_duration_ms = 0;
static uint32_t http_file_max_no_progress_ms = 0;
static char http_file_last_path[64] = {0};
static uint32_t wsstate_request_count = 0;
static uint32_t wsstate_low_heap_defer_count = 0;

static String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        switch (c) {
            case '&': out += F("&amp;"); break;
            case '<': out += F("&lt;"); break;
            case '>': out += F("&gt;"); break;
            case '"': out += F("&quot;"); break;
            default:  out += c; break;
        }
    }
    return out;
}


                                                            
static const char* kRestartMarkerPath = "/last_restart_marker.txt";
static String g_last_restart_marker_boot;

static void writeRestartMarker(const String& reason) {
                                                                                    
    File f = LittleFS.open(kRestartMarkerPath, "w");
    if (!f) return;

                                                                                 
    f.print("Reason: ");
    f.println(reason);

    f.print("Millis: ");
    f.println(millis());

    time_t nowt = time(nullptr);
    if (nowt > 100000) {
        struct tm* tm_info = localtime(&nowt);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
        f.print("Time: ");
        f.println(buf);
    }

    f.close();
}

static void loadRestartMarkerBoot() {
    File f = LittleFS.open(kRestartMarkerPath, "r");
    if (!f) return;
    g_last_restart_marker_boot = f.readString();
    f.close();

                                                                               
                                                                            
                                                                                
                                                  
    LittleFS.remove(kRestartMarkerPath);
}

static void requestRestart(const char* reason) {
                                          
    String r = reason ? String(reason) : String("unknown");
    writeRestartMarker(r);
    delay(50);
    ESP.restart();
}

                                                                            
                                                                       
void sarMarkedRestart(const char* reason)
{
    requestRestart(reason ? reason : "library restart");
}



#if defined(ESP8266)
 #ifndef SAR_BEARSSL_TIMEOUT_PATCH
  #error "Phase15-Harden3 requires patch_bearssl_timeout.py (timeout preservation patch missing)"
 #endif
 #ifndef SAR_BEARSSL_SERVICE_HOOK_PATCH
  #error "Phase15-Harden3 requires patch_bearssl_timeout.py (pump service hook patch missing)"
 #endif
 #include <bearssl/bearssl.h>                
 extern "C" {
  #include <user_interface.h>                                      
 }

                                  
                                                             
   
                                                                          
                                                                           
                                                                              
                                                                                
                                                                
                                  
 static volatile bool sar_cloud_tls_handshake_in_progress = false;
 static uint32_t sar_tls_pump_service_count = 0;
 static uint32_t sar_tls_pump_service_last_ms = 0;
 static uint32_t sar_tls_pump_service_max_gap_ms = 0;

 class SarCloudTlsClient : public BearSSL::WiFiClientSecureCtx
 {
 public:
     int connectTcpOnly(const IPAddress& ip, uint16_t port, uint32_t timeoutMs)
     {
         setTimeout(timeoutMs);
         return WiFiClient::connect(ip, port);
     }

     bool startTlsOnly(const char* hostName, uint32_t timeoutMs)
     {
                                                                            
                                                                                
                                                                              
                                                             
         setTimeout(timeoutMs);
         sar_cloud_tls_handshake_in_progress = true;
         const bool ok = _connectSSL(hostName);
         sar_cloud_tls_handshake_in_progress = false;
         return ok;
     }

     bool tcpConnectedOnly()
     {
         return WiFiClient::connected();
     }
 };

 static SarCloudTlsClient          tlsClientStatic;

                                                                        
                                                                              
 static BearSSL::X509List          tlsCaStatic(SAR_MQTT_CA_CERT);

                           
 static BearSSL::X509List          mqttCloudCaRootYeStatic(SAR_MQTT_CLOUD_ROOT_YE_CERT);

 static BearSSL::Session           mqttTlsSessionStatic;

                                                               
 static WiFiClient                 wifiClientPlainStatic;

                           
 BearSSL::WiFiClientSecureCtx *tlsClient = &tlsClientStatic;
 BearSSL::X509List            *tlsCa     = &mqttCloudCaRootYeStatic;

 static void generateBootId()
 {
     os_get_random(reinterpret_cast<unsigned char*>(&g_boot_id), sizeof(g_boot_id));
     if (g_boot_id == 0) g_boot_id = ESP.getChipId() ^ micros() ^ ESP.getCycleCount();
 }

 static String bootIdString()
 {
     char buf[9];
     snprintf(buf, sizeof(buf), "%08lX", (unsigned long)g_boot_id);
     return String(buf);
 }

 static void appendStructuredResetInfo(String& out)
 {
     const struct rst_info* ri = system_get_rst_info();
     if (!ri) {
         out += F("ResetStruct: unavailable\n");
         return;
     }

     char buf[192];
     snprintf_P(buf, sizeof(buf),
                PSTR("ResetStructReason: %u\nExceptionCause: %u\nEPC1: 0x%08lX\nEPC2: 0x%08lX\nEPC3: 0x%08lX\nEXCVADDR: 0x%08lX\nDEPC: 0x%08lX\n"),
                (unsigned)ri->reason,
                (unsigned)ri->exccause,
                (unsigned long)ri->epc1,
                (unsigned long)ri->epc2,
                (unsigned long)ri->epc3,
                (unsigned long)ri->excvaddr,
                (unsigned long)ri->depc);
     out += buf;
 }
#endif

                                                                               
#if defined(ESP8266)
 static PubSubClient mqttClientStatic(wifiClientPlainStatic);                  
#else
 static WiFiClient   wifiClientStatic;
 static PubSubClient mqttClientStatic(wifiClientStatic);
#endif




static bool g_mqtt_last_connect_ok = false;                                                                 

static String getMacClean()
{
    String mac = WiFi.macAddress();                         
    mac.replace(":", "");
    mac.replace("-", "");
    mac.toUpperCase();
    return mac;
}

static void sarHandleSerialProvisioning()
{
    static char line[96] = {0};
    static uint8_t pos = 0;

    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') continue;

        if (c == '\n') {
            line[pos] = '\0';
            pos = 0;

            if (strcmp(line, "SARPROV:INFO") == 0) {
                const String id = getMacClean();
                Serial.print(F("SARPROV:INFO DEVICE_ID="));
                Serial.print(id);
                Serial.print(F(" PROVISIONED="));
                Serial.println(cloudV2CredentialsProvisioned() ? F("1") : F("0"));
                continue;
            }

            static const char prefix[] = "SARPROV:SET:";
            if (strncmp(line, prefix, sizeof(prefix) - 1) == 0) {
                if (cloudV2CredentialsProvisioned()) {
                    Serial.println(F("SARPROV:ERR ALREADY_PROVISIONED"));
                    continue;
                }

                String secret(line + sizeof(prefix) - 1);
                secret.trim();
                if (!cloudV2CredentialsSaveSecret(secret)) {
                    Serial.println(F("SARPROV:ERR INVALID_SECRET"));
                    continue;
                }

                                                                                         
                                                                                  
                                                                                    
                if (!cloudV2CredentialsLoad()) {
                    Serial.println(F("SARPROV:ERR VERIFY_FAILED"));
                    continue;
                }

                                                                                       
                mqttPassword = cloudV2CredentialsSecret();
                Serial.print(F("SARPROV:OK DEVICE_ID="));
                Serial.print(getMacClean());
                Serial.println(F(" PROVISIONED=1"));
                continue;
            }

            if (line[0] != '\0' && strncmp(line, "SARPROV:", 8) == 0)
                Serial.println(F("SARPROV:ERR UNKNOWN_COMMAND"));
            continue;
        }

        if (pos < sizeof(line) - 1) {
            line[pos++] = c;
        } else {
            pos = 0;
            Serial.println(F("SARPROV:ERR LINE_TOO_LONG"));
        }
    }
}

                                                                    
                                                                       
                                                                         
static void sarSerialProvisioningBootWindow(uint32_t windowMs)
{
    if (cloudV2CredentialsProvisioned()) return;

    Serial.print(F("SARPROV:READY DEVICE_ID="));
    Serial.print(getMacClean());
    Serial.println(F(" PROVISIONED=0"));

    const uint32_t started = millis();
    while ((uint32_t)(millis() - started) < windowMs) {
        sarHandleSerialProvisioning();
        if (cloudV2CredentialsProvisioned()) {
            Serial.println(F("SARPROV:BOOT_WINDOW_DONE PROVISIONED=1"));
            return;
        }
        delay(5);
        yield();
    }

    Serial.println(F("SARPROV:BOOT_WINDOW_DONE PROVISIONED=0"));
}

static bool timeLooksValid() {
  time_t now = time(nullptr);
  return (now > 1700000000);                 
}

static void waitForValidTime(uint32_t maxWaitMs = 8000) {
  uint32_t t0 = millis();
  while (!timeLooksValid() && (millis() - t0) < maxWaitMs) {
    delay(10);
    yield();
  }
}


                                                                                
                                                  
                                                                                
                                                                              
                                                    
  
                                                                             
                                                                                
                                                              
                                                                                
static const char* SAR_MIGRATION_API_URL = SAR_PUBLIC_MIGRATION_API_URL;
static const char* SAR_MIGRATION_START_PATH = "/api/v2/migration/start";
static const char* SAR_MIGRATION_POLL_PATH  = "/api/v2/migration/poll";
static const char* SAR_MIGRATION_STATE_PATH = "/sar_migration_state.json";

static String sarMigrationToken;
static String sarMigrationSessionId;
static uint32_t sarMigrationNextAttemptMs = 0;
static uint8_t sarMigrationFailStreak = 0;
static int sarMigrationLastHttpCode = 0;

#if defined(ESP8266)
static BearSSL::WiFiClientSecure sarMigrationTlsClient;
static BearSSL::Session sarMigrationTlsSession;
#endif

static bool sarMigrationIsHex64(const String& value)
{
    if (value.length() != 64) return false;
    for (uint16_t i = 0; i < value.length(); i++) {
        const char c = value.charAt(i);
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool sarMigrationSessionLooksValid(const String& value)
{
    if (value.length() != 36) return false;
    return value.charAt(8) == '-' &&
           value.charAt(13) == '-' &&
           value.charAt(18) == '-' &&
           value.charAt(23) == '-';
}

static String sarMigrationRandomToken()
{
#if defined(ESP8266)
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    os_get_random(raw, sizeof(raw));

    static const char hex[] = "0123456789abcdef";
    char out[65];
    for (uint8_t i = 0; i < sizeof(raw); i++) {
        out[i * 2]     = hex[(raw[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[raw[i] & 0x0F];
    }
    out[64] = '\0';
    memset(raw, 0, sizeof(raw));
    return String(out);
#else
    return String();
#endif
}

static bool sarCloudV2MigrationSaveState()
{
    if (!sarMigrationIsHex64(sarMigrationToken)) return false;

    File file = LittleFS.open(SAR_MIGRATION_STATE_PATH, "w");
    if (!file) return false;

    StaticJsonDocument<192> doc;
    doc[F("v")] = 1;
    doc[F("token")] = sarMigrationToken;
    if (sarMigrationSessionId.length()) doc[F("sessionId")] = sarMigrationSessionId;

    const bool ok = serializeJson(doc, file) > 0;
    file.close();
    return ok;
}

static void sarCloudV2MigrationClearState()
{
    sarMigrationToken = "";
    sarMigrationSessionId = "";
    sarMigrationFailStreak = 0;
    sarMigrationNextAttemptMs = 0;
    sarMigrationLastHttpCode = 0;

    if (LittleFS.exists(SAR_MIGRATION_STATE_PATH)) {
        LittleFS.remove(SAR_MIGRATION_STATE_PATH);
    }
}

static void sarCloudV2MigrationLoadState()
{
    sarMigrationToken = "";
    sarMigrationSessionId = "";

    File file = LittleFS.open(SAR_MIGRATION_STATE_PATH, "r");
    if (!file) return;

    StaticJsonDocument<192> doc;
    const DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err || (doc[F("v")] | 0) != 1) {
        sarCloudV2MigrationClearState();
        return;
    }

    String token = doc[F("token")] | "";
    String sessionId = doc[F("sessionId")] | "";
    token.trim();
    token.toLowerCase();
    sessionId.trim();
    sessionId.toLowerCase();

    if (!sarMigrationIsHex64(token) ||
        (sessionId.length() && !sarMigrationSessionLooksValid(sessionId))) {
        sarCloudV2MigrationClearState();
        return;
    }

    sarMigrationToken = token;
    sarMigrationSessionId = sessionId;

    Serial.print(F("[CloudV2 MIG] restored temporary state; session="));
    Serial.println(sarMigrationSessionId.length() ? F("yes") : F("no"));
}

static uint32_t sarMigrationBackoffMs(bool rateLimited = false)
{
    if (sarMigrationFailStreak < 8) sarMigrationFailStreak++;

    if (rateLimited) return 30UL * 60UL * 1000UL;

    uint8_t shift = sarMigrationFailStreak > 6 ? 6 : sarMigrationFailStreak;
    uint32_t delayMs = 15000UL << shift;                       
    if (delayMs > 30UL * 60UL * 1000UL) delayMs = 30UL * 60UL * 1000UL;
    return delayMs;
}

static void sarMigrationScheduleRetry(bool rateLimited = false)
{
    sarMigrationNextAttemptMs = millis() + sarMigrationBackoffMs(rateLimited);
}

static void sarMigrationResetBackoff(uint32_t nextDelayMs)
{
    sarMigrationFailStreak = 0;
    sarMigrationNextAttemptMs = millis() + nextDelayMs;
}

static String sarMigrationJsonError(const String& payload)
{
    if (!payload.length()) return String();

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload)) return String();

    String err = doc[F("error")] | "";
    err.trim();
    return err;
}

static int sarMigrationHttpPost(const char* path, const String& payload, String& response)
{
#if !defined(ESP8266)
    (void)path; (void)payload; response = "";
    return -1;
#else
    response = "";

    if (WiFi.status() != WL_CONNECTED) return -1;
    if (!timeLooksValid()) return -2;

    sarMigrationTlsClient.stop();
    sarMigrationTlsClient.setTrustAnchors(&tlsCaStatic);
    sarMigrationTlsClient.setSession(&sarMigrationTlsSession);
    sarMigrationTlsClient.setBufferSizes(2048, 512);
    sarMigrationTlsClient.setTimeout(15);
    sarMigrationTlsClient.setX509Time(time(nullptr));

    String url;
    url.reserve(96);
    url += SAR_MIGRATION_API_URL;
    url += path;

                                                                         
                                                                          
    struct _SarMigrationHardFreezeGuard {
        bool active = false;
        _SarMigrationHardFreezeGuard() {
            if (bwc) {
                bwc->beginCloudPollingGuard(20000UL);
                pause_all(true);
                active = true;
            }
        }
        ~_SarMigrationHardFreezeGuard() {
            if (active) {
                pause_all(false);
                if (bwc) bwc->finishCloudPollingGuard(3000UL);
            }
        }
    } freezeGuard;

    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);
    http.useHTTP10(true);

    if (!http.begin(sarMigrationTlsClient, url)) {
        sarMigrationTlsClient.stop();
        return -3;
    }

    http.addHeader(F("Content-Type"), F("application/json"));
    http.addHeader(F("Accept"), F("application/json"));
    http.addHeader(F("Cache-Control"), F("no-store"));

    const int code = http.POST(payload);
    if (code > 0) response = http.getString();

    http.end();
    sarMigrationTlsClient.stop();
    return code;
#endif
}

static bool sarMigrationEnsureToken()
{
    if (sarMigrationIsHex64(sarMigrationToken)) return true;

    sarMigrationToken = sarMigrationRandomToken();
    sarMigrationSessionId = "";
    if (!sarMigrationIsHex64(sarMigrationToken)) {
        sarMigrationToken = "";
        return false;
    }

                                                                               
                                                         
    if (!sarCloudV2MigrationSaveState()) {
        sarMigrationToken = "";
        return false;
    }
    return true;
}

static bool sarMigrationStartSession()
{
    if (!sarMigrationEnsureToken()) {
        Serial.println(F("[CloudV2 MIG] could not create/persist device token"));
        return false;
    }

    StaticJsonDocument<192> requestDoc;
    requestDoc[F("protocol")] = 2;
    requestDoc[F("deviceId")] = getMacClean();
    requestDoc[F("deviceToken")] = sarMigrationToken;

    String request;
    request.reserve(160);
    serializeJson(requestDoc, request);

    String response;
    const int code = sarMigrationHttpPost(SAR_MIGRATION_START_PATH, request, response);
    sarMigrationLastHttpCode = code;

    if (code != 201) {
        const String err = sarMigrationJsonError(response);
        Serial.print(F("[CloudV2 MIG] start HTTP="));
        Serial.print(code);
        if (err.length()) {
            Serial.print(F(" error="));
            Serial.print(err);
        }
        Serial.println();

                                                                                
                                                                                     
        sarMigrationScheduleRetry(code == 429);
        return false;
    }

    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, response)) {
        Serial.println(F("[CloudV2 MIG] invalid /start JSON"));
        sarMigrationScheduleRetry();
        return false;
    }

    String sessionId = doc[F("sessionId")] | "";
    String status = doc[F("status")] | "";
    sessionId.trim();
    sessionId.toLowerCase();
    status.trim();

    if (!sarMigrationSessionLooksValid(sessionId) ||
        !(status == F("approved") || status == F("pending") || status == F("secret_issued"))) {
        Serial.println(F("[CloudV2 MIG] invalid /start response"));
        sarMigrationScheduleRetry();
        return false;
    }

    sarMigrationSessionId = sessionId;
    if (!sarCloudV2MigrationSaveState()) {
        Serial.println(F("[CloudV2 MIG] session could not be persisted"));
        sarMigrationScheduleRetry();
        return false;
    }

    uint32_t pollAfterMs = (uint32_t)(doc[F("pollAfterSeconds")] | 2UL) * 1000UL;
    if (pollAfterMs < 1000UL) pollAfterMs = 1000UL;
    if (pollAfterMs > 30000UL) pollAfterMs = 30000UL;
    sarMigrationResetBackoff(pollAfterMs);

    Serial.println(F("[CloudV2 MIG] bootstrap session approved"));
    return true;
}

static bool sarMigrationSaveReceivedSecret(String& secret)
{
    secret.trim();
    secret.toLowerCase();
    if (!sarMigrationIsHex64(secret)) return false;

    if (!cloudV2CredentialsSaveSecret(secret)) return false;

                                                            
    if (!cloudV2CredentialsLoad()) return false;

    mqttPassword = cloudV2CredentialsSecret();

                                                                               
                                                              
    sarPrepareFreshV2PairingAfterMigration();

                                                                 
    sarCloudV2MigrationClearState();

                                                                         
    for (uint16_t i = 0; i < secret.length(); i++) secret.setCharAt(i, '0');
    secret = "";

    enableMqtt = useMqtt;
    cloud_next_mqtt_try_ms = 0;
    startMqtt();

    Serial.println(F("[CloudV2 MIG] device secret stored and verified; V2 MQTT enabled"));
    return true;
}

static bool sarMigrationPollSession()
{
    if (!sarMigrationSessionLooksValid(sarMigrationSessionId) ||
        !sarMigrationIsHex64(sarMigrationToken)) return false;

    StaticJsonDocument<224> requestDoc;
    requestDoc[F("protocol")] = 2;
    requestDoc[F("sessionId")] = sarMigrationSessionId;
    requestDoc[F("deviceToken")] = sarMigrationToken;

    String request;
    request.reserve(192);
    serializeJson(requestDoc, request);

    String response;
    const int code = sarMigrationHttpPost(SAR_MIGRATION_POLL_PATH, request, response);
    sarMigrationLastHttpCode = code;

    if (code != 200) {
        const String err = sarMigrationJsonError(response);
        Serial.print(F("[CloudV2 MIG] poll HTTP="));
        Serial.print(code);
        if (err.length()) {
            Serial.print(F(" error="));
            Serial.print(err);
        }
        Serial.println();

        if (code == 409 && err == F("migration_session_expired")) {
            sarCloudV2MigrationClearState();
            sarMigrationNextAttemptMs = millis() + 30000UL;
        } else {
            sarMigrationScheduleRetry(code == 429);
        }
        return false;
    }

    DynamicJsonDocument doc(768);
    if (deserializeJson(doc, response)) {
        Serial.println(F("[CloudV2 MIG] invalid /poll JSON"));
        sarMigrationScheduleRetry();
        return false;
    }

    String status = doc[F("status")] | "";
    status.trim();

    if (status == F("completed")) {
                                                                            
                                                                 
        Serial.println(F("[CloudV2 MIG] server says completed but local secret is absent"));
        sarMigrationScheduleRetry(true);
        return false;
    }

    if (status != F("secret_issued")) {
        sarMigrationScheduleRetry();
        return false;
    }

    String responseDeviceId = doc[F("deviceId")] | "";
    String secret = doc[F("secret")] | "";
    String mqttHost = doc[F("mqtt")][F("host")] | "";
    String mqttUsername = doc[F("mqtt")][F("username")] | "";
    const uint16_t mqttResponsePort = doc[F("mqtt")][F("port")] | 0;

    responseDeviceId.trim(); responseDeviceId.toUpperCase();
    mqttHost.trim();
    mqttUsername.trim(); mqttUsername.toUpperCase();

    const String ownDeviceId = getMacClean();
    if (responseDeviceId != ownDeviceId ||
        mqttUsername != ownDeviceId ||
        !mqttHost.equalsIgnoreCase(String(SAR_CLOUD_HOST)) ||
        mqttResponsePort != SAR_CLOUD_PORT ||
        !sarMigrationIsHex64(secret)) {
        Serial.println(F("[CloudV2 MIG] /poll credential metadata mismatch"));
        for (uint16_t i = 0; i < secret.length(); i++) secret.setCharAt(i, '0');
        secret = "";
        sarMigrationScheduleRetry();
        return false;
    }

    if (!sarMigrationSaveReceivedSecret(secret)) {
        Serial.println(F("[CloudV2 MIG] EEPROM save/verify failed"));
        for (uint16_t i = 0; i < secret.length(); i++) secret.setCharAt(i, '0');
        secret = "";
        sarMigrationScheduleRetry();
        return false;
    }

    return true;
}

static void sarCloudV2MigrationTick()
{
                                                                   
    if (cloudV2CredentialsProvisioned()) {
        if (sarMigrationToken.length() || sarMigrationSessionId.length() ||
            LittleFS.exists(SAR_MIGRATION_STATE_PATH)) {
            sarCloudV2MigrationClearState();
        }
        return;
    }

                                                                            
                                                         
    if (!mqttCloudMode || !useMqtt) return;
    if (WiFi.status() != WL_CONNECTED) return;

    const uint32_t now = millis();
    if (sarMigrationNextAttemptMs != 0 &&
        (int32_t)(now - sarMigrationNextAttemptMs) < 0) return;

    if (!timeLooksValid()) {
        sarMigrationNextAttemptMs = now + 5000UL;
        return;
    }

#if defined(ESP8266)
    if (ESP.getFreeHeap() < 12000UL || ESP.getMaxFreeBlockSize() < 6500UL) {
        sarMigrationNextAttemptMs = now + 30000UL;
        return;
    }
#endif

    if (!sarMigrationSessionId.length()) {
        sarMigrationStartSession();
    } else {
        sarMigrationPollSession();
    }
}


                       
                                    
                       
static const uint32_t PRESENCE_POLL_OFFLINE_MS  = 3UL * 60UL * 1000UL;              
static const uint32_t PRESENCE_POLL_BURST_MS = 20000UL;               
static const uint32_t PRESENCE_POLL_ONLINE_MS = 60000UL;               
static const uint32_t PRESENCE_ACTIVE_WINDOW_MS = 3UL  * 60UL * 1000UL;              
static const uint32_t PRESENCE_GRACE_MS         = 45UL * 1000UL;                       

                                   
                                                     
static const bool PRESENCE_DEBUG = false;
static const bool PRESENCE_FORCE_PUBLIC_DNS = true;
static const IPAddress PRESENCE_DNS_PRIMARY(1, 1, 1, 1);
static const IPAddress PRESENCE_DNS_SECONDARY(8, 8, 8, 8);

static bool presence_last_dns_ok = false;
static IPAddress presence_last_resolved_ip(0,0,0,0);
static bool presence_last_connect_ok = false;
static bool presence_last_ip_fallback_used = false;
static bool presence_last_force_ip_mode = false;
static uint8_t presence_host_anchor_fail_streak = 0;
static uint32_t presence_ip_preferred_until_ms = 0;
static int  presence_last_ssl_err = 0;
static bool presence_last_allowed_value = false;
static int  presence_last_body_byte = -1;
static String presence_last_http_status_line;
static bool presence_last_mqtt_should_run = false;
static uint32_t presence_last_stale_guard_age_ms = 0;
static uint32_t presence_stale_guard_count = 0;
static bool presence_cost_guard_active = false;
static uint32_t presence_cost_guard_until_ms = 0;
static uint32_t presence_low_heap_skip_count = 0;
static uint32_t presence_last_skip_heap_free = 0;
static uint32_t presence_last_skip_heap_block = 0;
static String presence_diag_log;
static String presence_diag_boot;
static const char* kPresenceDiagPath = "/last_presence_diag.txt";

                                            
                                                                                      
static uint32_t presence_poll_seq = 0;
static uint32_t presence_last_poll_start_ms = 0;
static uint32_t presence_last_poll_duration_ms = 0;
static uint32_t presence_last_dns_duration_ms = 0;
static uint32_t presence_last_ntp_wait_ms = 0;
static uint32_t presence_last_tls_connect_ms = 0;
static uint32_t presence_last_http_send_ms = 0;
static uint32_t presence_last_wait_first_byte_ms = 0;
static uint32_t presence_last_status_read_ms = 0;
static uint32_t presence_last_header_read_ms = 0;
static uint32_t presence_last_body_read_ms = 0;
static uint32_t presence_last_http_duration_ms = 0;
static uint32_t presence_last_heap_start = 0;
static uint32_t presence_last_heap_end = 0;
static uint32_t presence_last_heap_min = 0;
static uint32_t presence_last_block_start = 0;
static uint32_t presence_last_block_end = 0;
static uint32_t presence_last_block_min = 0;
static uint32_t presence_last_frag_start = 0;
static uint32_t presence_last_frag_end = 0;
static uint32_t presence_last_bytes_read = 0;
static int32_t  presence_last_content_len = -1;
static uint8_t  presence_last_dns_tries = 0;
static uint8_t  presence_last_tls_mode = 0;                                 
static uint8_t  presence_last_outcome = 0;                                                                                                  
static char     presence_last_stage[18] = "boot";
static String   presence_crash_marker_boot;
static const char* kPresenceCrashMarkerPath = "/presence_crash_marker.txt";

static void presenceAdaptiveRaiseBlockMin(uint32_t observedBlock, const __FlashStringHelper* reason);

static void presenceTrackHeap()
{
#if defined(ESP8266)
    uint32_t fh = ESP.getFreeHeap();
    uint32_t mb = ESP.getMaxFreeBlockSize();
    if (presence_last_heap_min == 0 || fh < presence_last_heap_min) presence_last_heap_min = fh;
    if (presence_last_block_min == 0 || mb < presence_last_block_min) presence_last_block_min = mb;
#endif
}

static void presenceSetStage(const char* stage, bool persist = false)
{
    if (!stage) stage = "?";
    strncpy(presence_last_stage, stage, sizeof(presence_last_stage) - 1);
    presence_last_stage[sizeof(presence_last_stage) - 1] = '\0';
    presenceTrackHeap();
    if (persist) {
        File f = LittleFS.open(kPresenceCrashMarkerPath, "w");
        if (f) {
            f.print(F("seq=")); f.println(presence_poll_seq);
            f.print(F("stage=")); f.println(presence_last_stage);
            f.print(F("millis=")); f.println(millis());
            f.print(F("heap=")); f.println(ESP.getFreeHeap());
            f.print(F("maxBlock=")); f.println(ESP.getMaxFreeBlockSize());
            f.print(F("frag=")); f.println(ESP.getHeapFragmentation());
            f.print(F("wifi=")); f.println((WiFi.status() == WL_CONNECTED) ? F("connected") : F("not_connected"));
            f.print(F("rssi=")); f.println(WiFi.RSSI());
            f.close();
        }
    }
}

static void loadPresenceCrashMarkerBoot()
{
    File f = LittleFS.open(kPresenceCrashMarkerPath, "r");
    if (!f) return;
    presence_crash_marker_boot = f.readString();
    f.close();

                                                                      
                                                                  
                                                                        
                                                      
    if (presence_crash_marker_boot.indexOf(F("stage=tls_host")) >= 0 ||
        presence_crash_marker_boot.indexOf(F("stage=tls_ip")) >= 0 ||
        presence_crash_marker_boot.indexOf(F("stage=tls")) >= 0) {
        presenceAdaptiveRaiseBlockMin(7000UL, F("boot_tls_crash"));
    }
}

static void appendPresenceDiag(const String& line, bool persist = false)
{
    if (presence_diag_log.length() > 1800) {
        int cut = presence_diag_log.indexOf('\n', presence_diag_log.length() - 1200);
        if (cut > 0) presence_diag_log.remove(0, cut + 1);
    }
    presence_diag_log += line;
    presence_diag_log += '\n';
    if (persist) {
        File f = LittleFS.open(kPresenceDiagPath, "w");
        if (f) { f.print(presence_diag_log); f.close(); }
    }
}

static void loadPresenceDiagBoot()
{
    File f = LittleFS.open(kPresenceDiagPath, "r");
    if (!f) return;
    presence_diag_boot = f.readString();
    f.close();
}



static const char* PRESENCE_HOST = SAR_PUBLIC_PRESENCE_HOST;
static const uint16_t PRESENCE_PORT = 443;
static const char* PRESENCE_PATH_PREFIX = "/presence?deviceId=";

static uint32_t presence_active_until_ms = 0;

                                            
#if defined(ESP8266)
static BearSSL::WiFiClientSecure presenceClient;
extern const char PRESENCE_ROOT_CA[] PROGMEM;
static BearSSL::X509List presenceCaStatic(PRESENCE_ROOT_CA);
static BearSSL::X509List* presenceCa = &presenceCaStatic;
static BearSSL::Session presenceSession;





                                                                         
                                                                                                 
                                                                        
const char PRESENCE_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFCzCCAvOgAwIBAgIQf/AFoHxM3tEArZ1mpRB7mDANBgkqhkiG9w0BAQsFADBH
MQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExM
QzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMjMxMjEzMDkwMDAwWhcNMjkwMjIw
MTQwMDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRydXN0IFNl
cnZpY2VzMQwwCgYDVQQDEwNXUjIwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK
AoIBAQCp/5x/RR5wqFOfytnlDd5GV1d9vI+aWqxG8YSau5HbyfsvAfuSCQAWXqAc
+MGr+XgvSszYhaLYWTwO0xj7sfUkDSbutltkdnwUxy96zqhMt/TZCPzfhyM1IKji
aeKMTj+xWfpgoh6zySBTGYLKNlNtYE3pAJH8do1cCA8Kwtzxc2vFE24KT3rC8gIc
LrRjg9ox9i11MLL7q8Ju26nADrn5Z9TDJVd06wW06Y613ijNzHoU5HEDy01hLmFX
xRmpC5iEGuh5KdmyjS//V2pm4M6rlagplmNwEmceOuHbsCFx13ye/aoXbv4r+zgX
FNFmp6+atXDMyGOBOozAKql2N87jAgMBAAGjgf4wgfswDgYDVR0PAQH/BAQDAgGG
MB0GA1UdJQQWMBQGCCsGAQUFBwMBBggrBgEFBQcDAjASBgNVHRMBAf8ECDAGAQH/
AgEAMB0GA1UdDgQWBBTeGx7teRXUPjckwyG77DQ5bUKyMDAfBgNVHSMEGDAWgBTk
rysmcRorSCeFL1JmLO/wiRNxPjA0BggrBgEFBQcBAQQoMCYwJAYIKwYBBQUHMAKG
GGh0dHA6Ly9pLnBraS5nb29nL3IxLmNydDArBgNVHR8EJDAiMCCgHqAchhpodHRw
Oi8vYy5wa2kuZ29vZy9yL3IxLmNybDATBgNVHSAEDDAKMAgGBmeBDAECATANBgkq
hkiG9w0BAQsFAAOCAgEARXWL5R87RBOWGqtY8TXJbz3S0DNKhjO6V1FP7sQ02hYS
TL8Tnw3UVOlIecAwPJQl8hr0ujKUtjNyC4XuCRElNJThb0Lbgpt7fyqaqf9/qdLe
SiDLs/sDA7j4BwXaWZIvGEaYzq9yviQmsR4ATb0IrZNBRAq7x9UBhb+TV+PfdBJT
DhEl05vc3ssnbrPCuTNiOcLgNeFbpwkuGcuRKnZc8d/KI4RApW//mkHgte8y0YWu
ryUJ8GLFbsLIbjL9uNrizkqRSvOFVU6xddZIMy9vhNkSXJ/UcZhjJY1pXAprffJB
vei7j+Qi151lRehMCofa6WBmiA4fx+FOVsV2/7R6V2nyAiIJJkEd2nSi5SnzxJrl
Xdaqev3htytmOPvoKWa676ATL/hzfvDaQBEcXd2Ppvy+275W+DKcH0FBbX62xevG
iza3F4ydzxl6NJ8hk8R+dDXSqv1MbRT1ybB5W0k8878XSOjvmiYTDIfyc9acxVJr
Y/cykHipa+te1pOhv7wYPYtZ9orGBV5SGOJm4NrB3K1aJar0RfzxC3ikr7Dyc6Qw
qDTBU39CluVIQeuQRgwG3MuSxl7zRERDRilGoKb8uY45JzmxWuKxrfwT/478JuHU
/oTxUFqOl2stKnn7QGTq8z29W+GgBLCXSBxC9epaHM0myFH/FJlniXJfHeytWt0=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFYjCCBEqgAwIBAgIQd70NbNs2+RrqIQ/E8FjTDTANBgkqhkiG9w0BAQsFADBX
MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE
CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIwMDYx
OTAwMDA0MloXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT
GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFIx
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAthECix7joXebO9y/lD63
ladAPKH9gvl9MgaCcfb2jH/76Nu8ai6Xl6OMS/kr9rH5zoQdsfnFl97vufKj6bwS
iV6nqlKr+CMny6SxnGPb15l+8Ape62im9MZaRw1NEDPjTrETo8gYbEvs/AmQ351k
KSUjB6G00j0uYODP0gmHu81I8E3CwnqIiru6z1kZ1q+PsAewnjHxgsHA3y6mbWwZ
DrXYfiYaRQM9sHmklCitD38m5agI/pboPGiUU+6DOogrFZYJsuB6jC511pzrp1Zk
j5ZPaK49l8KEj8C8QMALXL32h7M1bKwYUH+E4EzNktMg6TO8UpmvMrUpsyUqtEj5
cuHKZPfmghCN6J3Cioj6OGaK/GP5Afl4/Xtcd/p2h/rs37EOeZVXtL0m79YB0esW
CruOC7XFxYpVq9Os6pFLKcwZpDIlTirxZUTQAs6qzkm06p98g7BAe+dDq6dso499
iYH6TKX/1Y7DzkvgtdizjkXPdsDtQCv9Uw+wp9U7DbGKogPeMa3Md+pvez7W35Ei
Eua++tgy/BBjFFFy3l3WFpO9KWgz7zpm7AeKJt8T11dleCfeXkkUAKIAf5qoIbap
sZWwpbkNFhHax2xIPEDgfg1azVY80ZcFuctL7TlLnMQ/0lUTbiSw1nH69MG6zO0b
9f6BQdgAmD06yK56mDcYBZUCAwEAAaOCATgwggE0MA4GA1UdDwEB/wQEAwIBhjAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTkrysmcRorSCeFL1JmLO/wiRNxPjAf
BgNVHSMEGDAWgBRge2YaRQ2XyolQL30EzTSo//z9SzBgBggrBgEFBQcBAQRUMFIw
JQYIKwYBBQUHMAGGGWh0dHA6Ly9vY3NwLnBraS5nb29nL2dzcjEwKQYIKwYBBQUH
MAKGHWh0dHA6Ly9wa2kuZ29vZy9nc3IxL2dzcjEuY3J0MDIGA1UdHwQrMCkwJ6Al
oCOGIWh0dHA6Ly9jcmwucGtpLmdvb2cvZ3NyMS9nc3IxLmNybDA7BgNVHSAENDAy
MAgGBmeBDAECATAIBgZngQwBAgIwDQYLKwYBBAHWeQIFAwIwDQYLKwYBBAHWeQIF
AwMwDQYJKoZIhvcNAQELBQADggEBADSkHrEoo9C0dhemMXoh6dFSPsjbdBZBiLg9
NR3t5P+T4Vxfq7vqfM/b5A3Ri1fyJm9bvhdGaJQ3b2t6yMAYN/olUazsaL+yyEn9
WprKASOshIArAoyZl+tJaox118fessmXn1hIVw41oeQa1v1vg4Fv74zPl6/AhSrw
9U5pCZEt4Wi4wStz6dTZ/CLANx8LZh1J7QJVj2fhMtfTJr9w4z30Z209fOU0iOMy
+qduBmpvvYuR7hZL6Dupszfnw0Skfths18dG9ZKb59UhvmaSGZRVbNQpsg3BZlvi
d0lIKO2d1xozclOzgjXPYovJJIultzkMu34qQb9Sz/yilrbCgj8=
-----END CERTIFICATE-----
)EOF";


static bool presenceTlsReady = false;
static bool presenceMflnProbed = false;
static bool presenceMflnOk = false;
static uint16_t presenceRxBuf = 4096;
static uint16_t presenceTxBuf = 512;
static const uint32_t PRESENCE_TLS_TIMEOUT_MS      = 15000UL;
static const uint32_t PRESENCE_WAIT_FIRST_BYTE_MS  = 5000UL;
static const uint32_t PRESENCE_WAIT_STATUS_MS      = 5000UL;
static const uint32_t PRESENCE_WAIT_HEADER_MS      = 5000UL;
static const uint32_t PRESENCE_WAIT_BODY_MS        = 5000UL;
                                                                                       
                                                                                
                                                                        
                                                                 
static const uint32_t PRESENCE_HEAP_SKIP_FREE_MS          = 11000UL;
static const uint32_t PRESENCE_HEAP_SKIP_BLOCK_START_MS   = 5800UL;
static const uint32_t PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS   = 5800UL;
static const uint32_t PRESENCE_HEAP_SKIP_BLOCK_CEIL_MS    = 6600UL;
static const uint32_t PRESENCE_HEAP_SKIP_ADAPT_STEP_MS    = 200UL;
static const uint32_t PRESENCE_HEAP_SKIP_ADAPT_COOLDOWN_MS = 120000UL;
static const uint32_t PRESENCE_HEAP_SKIP_STALL_REARM_MS     = 10UL * 60UL * 1000UL;
static const uint32_t PRESENCE_FORCE_IP_WINDOW_MS         = 6UL * 60UL * 60UL * 1000UL;

static uint32_t presence_heap_skip_block_min_current = PRESENCE_HEAP_SKIP_BLOCK_START_MS;
static uint32_t presence_adaptive_last_adjust_ms = 0;
static uint32_t presence_adaptive_last_start_block = 0;
static uint32_t presence_adaptive_last_start_heap = 0;
static uint16_t presence_adaptive_near_skip_count = 0;
static uint16_t presence_adaptive_success_count = 0;
static uint16_t presence_adaptive_lower_count = 0;
static uint16_t presence_adaptive_raise_count = 0;

static void presenceAdaptiveLowerBlockMin(uint32_t observedBlock, const __FlashStringHelper* reason)
{
  uint32_t now = millis();
  if ((int32_t)(now - presence_adaptive_last_adjust_ms) < (int32_t)PRESENCE_HEAP_SKIP_ADAPT_COOLDOWN_MS) return;
  if (presence_heap_skip_block_min_current <= PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS) return;

  uint32_t oldMin = presence_heap_skip_block_min_current;
  uint32_t newMin = oldMin;
  if (observedBlock >= PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS && observedBlock < oldMin) {
    newMin = observedBlock;
  } else if (oldMin > PRESENCE_HEAP_SKIP_ADAPT_STEP_MS) {
    newMin = oldMin - PRESENCE_HEAP_SKIP_ADAPT_STEP_MS;
  }
  if (newMin < PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS) newMin = PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS;
  if (newMin >= oldMin) return;

  presence_heap_skip_block_min_current = newMin;
  presence_adaptive_last_adjust_ms = now;
  presence_adaptive_near_skip_count = 0;
  presence_adaptive_success_count = 0;
  presence_adaptive_lower_count++;

  appendPresenceDiag(String(F("adaptive blockMin lower ")) + oldMin + F("->") + newMin + F(" obs=") + observedBlock + F(" reason=") + reason, true);
}

static void presenceAdaptiveRaiseBlockMin(uint32_t observedBlock, const __FlashStringHelper* reason)
{
  uint32_t now = millis();
  uint32_t oldMin = presence_heap_skip_block_min_current;
  uint32_t newMin = oldMin + PRESENCE_HEAP_SKIP_ADAPT_STEP_MS;
  if (newMin < observedBlock + PRESENCE_HEAP_SKIP_ADAPT_STEP_MS) newMin = observedBlock + PRESENCE_HEAP_SKIP_ADAPT_STEP_MS;
  if (newMin > PRESENCE_HEAP_SKIP_BLOCK_CEIL_MS) newMin = PRESENCE_HEAP_SKIP_BLOCK_CEIL_MS;
  if (newMin <= oldMin) return;

  presence_heap_skip_block_min_current = newMin;
  presence_adaptive_last_adjust_ms = now;
  presence_adaptive_near_skip_count = 0;
  presence_adaptive_success_count = 0;
  presence_adaptive_raise_count++;

  appendPresenceDiag(String(F("adaptive blockMin raise ")) + oldMin + F("->") + newMin + F(" obs=") + observedBlock + F(" reason=") + reason, true);
}

static void initPresenceTlsOnce()
{
  if (presenceTlsReady) return;

  if (!presenceCa) {
    presenceCa = new BearSSL::X509List(PRESENCE_ROOT_CA);
  }

                             
  waitForValidTime(8000);

  presenceClient.setTrustAnchors(presenceCa);             
  presenceClient.setSession(&presenceSession);                              
  presenceClient.setTimeout((int)(PRESENCE_TLS_TIMEOUT_MS / 1000UL));
  presenceClient.setX509Time(time(nullptr));

                                                                                            
  if (!presenceMflnProbed) {
    presenceMflnProbed = true;
    presenceMflnOk = presenceClient.probeMaxFragmentLength(PRESENCE_HOST, PRESENCE_PORT, 1024);
    if (presenceMflnOk) {
      presenceRxBuf = 1024;
      presenceTxBuf = 512;
    } else {
      presenceRxBuf = 4096;
      presenceTxBuf = 512;
    }
  }

  presenceClient.setBufferSizes(presenceRxBuf, presenceTxBuf);

  if (PRESENCE_DEBUG) {
    Serial.printf_P(PSTR("PRESENCE TLS init: rx=%u tx=%u mfln=%d time=%lu\n"),
                    presenceRxBuf,
                    presenceTxBuf,
                    presenceMflnOk ? 1 : 0,
                    (unsigned long)time(nullptr));
  }

  presenceTlsReady = true;
}


#endif



static uint32_t presence_next_poll_ms = 0;
static uint32_t presence_grace_until_ms = 0;
static bool     presence_allowed = false;

                                                                       
static uint32_t cloud_pair_bootstrap_until_ms = 0;

#if defined(ESP8266)
                                          
static uint32_t mqtt_last_connected_ms = 0;
static uint32_t mqtt_last_attempt_ms   = 0;
static uint8_t  mqtt_fail_streak       = 0;
static uint32_t mqtt_stack_reset_ms    = 0;

                                                                              
static uint32_t wifi_got_ip_count = 0;
static uint32_t wifi_disconnect_count = 0;
static uint32_t wifi_last_got_ip_ms = 0;
static uint32_t wifi_last_disconnect_ms = 0;
static uint8_t  wifi_last_disconnect_reason = 0;
static uint32_t got_ip_heap_before = 0;
static uint32_t got_ip_heap_after = 0;
static int32_t  got_ip_heap_delta = 0;
static uint32_t got_ip_block_before = 0;
static uint32_t got_ip_block_after = 0;
static int32_t  got_ip_block_delta = 0;
static uint32_t http_init_count = 0;
static uint32_t http_reuse_count = 0;
static uint32_t ws_init_count = 0;
static uint32_t ws_reuse_count = 0;
static uint32_t mqtt_init_count = 0;
static uint32_t mqtt_reconfigure_count = 0;
static bool     mqtt_stack_initialized = false;
static uint32_t tls_admission_blocked_count = 0;
static uint32_t tls_admission_last_heap = 0;
static uint32_t tls_admission_last_block = 0;
static uint32_t tls_reclaim_count = 0;
static uint32_t tls_reclaim_heap_before = 0;
static uint32_t tls_reclaim_heap_after = 0;
static uint32_t tls_reclaim_block_before = 0;
static uint32_t tls_reclaim_block_after = 0;
static uint32_t tls_time_deferred_count = 0;
static uint32_t mqtt_last_connect_duration_ms = 0;
static uint32_t mqtt_last_connect_heap_before = 0;
static uint32_t mqtt_last_connect_heap_after = 0;
static uint32_t mqtt_last_connect_block_before = 0;
static uint32_t mqtt_last_connect_block_after = 0;
static uint32_t mqtt_connect_attempt_count = 0;
static uint32_t mqtt_connect_success_count = 0;
static uint32_t mqtt_connect_fail_count = 0;
static uint32_t mqtt_last_connect_success_ms_diag = 0;
static uint32_t mqtt_loop_drop_count = 0;
static uint32_t mqtt_last_loop_drop_ms = 0;
static int      mqtt_last_loop_drop_state = 999;
static const char* mqtt_last_connect_trigger = "boot";

                                                                              
                                                                                
                                                                               
                                                                          
                                                                            
                                                                 
static const uint32_t SAR_CLOUD_TCP_STAGE_TIMEOUT_MS  = 1800UL;
static const uint32_t SAR_MQTT_TLS_CONNECT_TIMEOUT_MS = 8000UL;
static const uint32_t SAR_MQTT_TLS_STOP_TIMEOUT_MS    = 250UL;
static const uint32_t SAR_MQTT_TLS_RUNTIME_TIMEOUT_MS = 1000UL;
static const uint16_t SAR_MQTT_CONNACK_TIMEOUT_S      = 2U;
static const uint32_t SAR_MQTT_STAGE_GAP_MS           = 25UL;
static const uint32_t SAR_CLOUD_DNS_STAGE_TIMEOUT_MS  = 1500UL;
static const uint32_t SAR_CLOUD_DNS_CACHE_VALID_MS        = 60000UL;
static const uint32_t SAR_TLS_ADMISSION_MIN_HEAP      = 12000UL;
static const uint32_t SAR_TLS_ADMISSION_MIN_BLOCK     = 7000UL;
static const uint32_t SAR_TLS_ADMISSION_RETRY_MS      = 30000UL;
static const uint32_t SAR_TLS_ADMISSION_RECOVERY_MS   = 5UL * 60UL * 1000UL;
static const uint32_t SAR_MQTT_SLOW_CONNECT_MS        = 3000UL;

static uint32_t tls_admission_blocked_since_ms = 0;
static uint32_t tls_admission_last_blocked_ms = 0;
static uint32_t tls_admission_recovery_restarts = 0;
static uint32_t mqtt_tls_timeout_reapply_count = 0;
static uint32_t mqtt_tls_stop_count = 0;
static uint32_t mqtt_tls_last_stop_duration_ms = 0;
static uint32_t mqtt_tls_max_stop_duration_ms = 0;
static uint32_t mqtt_slow_connect_count = 0;
static uint32_t mqtt_over_5s_connect_count = 0;
static bool     cloud_mqtt_attempt_in_progress = false;
static uint32_t cloud_mqtt_stage_not_before_ms = 0;
static uint32_t mqtt_tls_preconnect_attempt_count = 0;
static uint32_t mqtt_tls_preconnect_success_count = 0;
static uint32_t mqtt_tls_preconnect_fail_count = 0;
static uint32_t mqtt_tls_preconnect_last_duration_ms = 0;
static uint32_t mqtt_tls_preconnect_max_duration_ms = 0;
static uint32_t mqtt_tls_preconnect_heap_before = 0;
static uint32_t mqtt_tls_preconnect_heap_after = 0;
static uint32_t mqtt_tls_preconnect_block_before = 0;
static uint32_t mqtt_tls_preconnect_block_after = 0;
static uint32_t mqtt_tls_preconnect_slow_count = 0;
static uint32_t mqtt_tls_preconnect_over4s_count = 0;
static uint32_t cloud_dns_preflight_valid_until_ms = 0;
static IPAddress cloud_dns_preflight_ip;
static uint32_t cloud_dns_preflight_attempt_count = 0;
static uint32_t cloud_dns_preflight_success_count = 0;
static uint32_t cloud_dns_preflight_fail_count = 0;
static uint32_t cloud_dns_preflight_last_duration_ms = 0;
static uint32_t cloud_dns_preflight_max_duration_ms = 0;
                                                                         
static bool     cloud_tcp_stage_ready = false;
static uint32_t mqtt_tcp_preconnect_attempt_count = 0;
static uint32_t mqtt_tcp_preconnect_success_count = 0;
static uint32_t mqtt_tcp_preconnect_fail_count = 0;
static uint32_t mqtt_tcp_preconnect_last_duration_ms = 0;
static uint32_t mqtt_tcp_preconnect_max_duration_ms = 0;
static uint32_t mqtt_tcp_preconnect_heap_before = 0;
static uint32_t mqtt_tcp_preconnect_heap_after = 0;
static uint32_t mqtt_tcp_preconnect_block_before = 0;
static uint32_t mqtt_tcp_preconnect_block_after = 0;

                                                                   
                                                                          
                                                             
static void cloudTlsStopBounded(bool gracefulMqttDisconnect, const char* reason)
{
    if (!mqttCloudMode || !tlsClient) {
        if (aWifiClient) aWifiClient->stop();
        return;
    }

                                                                     
    cloud_mqtt_attempt_in_progress = false;
    cloud_mqtt_stage_not_before_ms = 0;
    cloud_tcp_stage_ready = false;

    const uint32_t t0 = millis();
    mqtt_tls_stop_count++;

                                                                             
                                                                             
                          
    tlsClient->setTimeout(SAR_MQTT_TLS_STOP_TIMEOUT_MS);
    mqtt_tls_timeout_reapply_count++;
                                                                            
                                                                              
                                                                              
    if (gracefulMqttDisconnect && mqttClient && mqttClient->state() == MQTT_CONNECTED) {
        mqttClient->disconnect();
    }

                                                                              
                                                                             
                                                                      
    tlsClient->setTimeout(SAR_MQTT_TLS_STOP_TIMEOUT_MS);
    mqtt_tls_timeout_reapply_count++;
    (void)tlsClient->stop(50U);
    tlsClient->setTimeout(SAR_MQTT_TLS_CONNECT_TIMEOUT_MS);
    mqtt_tls_timeout_reapply_count++;

    const uint32_t dt = (uint32_t)(millis() - t0);
    mqtt_tls_last_stop_duration_ms = dt;
    if (dt > mqtt_tls_max_stop_duration_ms) mqtt_tls_max_stop_duration_ms = dt;

    if (reason && dt > 500UL) {
        Serial.printf_P(PSTR("[TLS] bounded stop reason=%s took=%lu ms\n"),
                        reason, (unsigned long)dt);
    }
}

static void cloudMqttConnectFailure(const char* stage)
{
    g_mqtt_last_connect_ok = false;
    mqtt_connect_fail_count++;
    if (mqtt_fail_streak < 250) mqtt_fail_streak++;
    cloud_mqtt_attempt_in_progress = false;

                                                                             
                                                 
    cloudTlsStopBounded(false, stage ? stage : "cloud-connect-failed");

    uint32_t backoffMs = 120000UL;
    if (mqtt_fail_streak <= 1) backoffMs = 15000UL;
    else if (mqtt_fail_streak == 2) backoffMs = 30000UL;
    else if (mqtt_fail_streak == 3) backoffMs = 60000UL;
    cloud_next_mqtt_try_ms = millis() + backoffMs;

    Serial.printf_P(PSTR("[TLS] %s failure streak=%u -> retry in %lu ms\n"),
                    stage ? stage : "connect",
                    (unsigned)mqtt_fail_streak, (unsigned long)backoffMs);
}
#endif


static inline bool cloudBootstrapActive()
{
                                                           
                                                                             
                                                                                               
  if (cloud_pair_bootstrap_until_ms == 0) return false;

  const bool active = ((int32_t)(cloud_pair_bootstrap_until_ms - millis()) > 0);
  if (!active) cloud_pair_bootstrap_until_ms = 0;
  return active;
}


                                                   
static uint32_t addJitter(uint32_t baseMs) {
                
  int32_t j = (int32_t)(baseMs / 10);
  int32_t r = (int32_t)random(-j, j + 1);
  int32_t out = (int32_t)baseMs + r;
  if (out < 1000) out = 1000;                
  return (uint32_t)out;
}

                                                    
static uint32_t presence_last_use_ms = 0;
static uint32_t presence_conn_open_ms = 0;

                                                                                       
static const uint32_t PRESENCE_CONN_MAX_AGE_MS  = 120000UL;              

                                                                           
static const uint32_t PRESENCE_CONN_IDLE_CLOSE_MS = 30000UL;               


                                                                      
static bool presence_polling_paused = false;

                                                                 
static uint32_t sar_cloud_next_telemetry_ms = 0;


                                                                                
                                      
                                                                                    
                                                                                
static uint32_t bwc_diag_last_loop_ms = 0;
static uint32_t bwc_diag_last_gap_ms = 0;
static uint32_t bwc_diag_max_gap_ms = 0;
static uint32_t bwc_diag_gap_over_250 = 0;
static uint32_t bwc_diag_gap_over_500 = 0;
static uint32_t bwc_diag_gap_over_1000 = 0;
static uint32_t bwc_diag_gap_over_3000 = 0;

static bool pause_all_diag_active = false;
static uint32_t pause_all_diag_started_ms = 0;
static uint32_t pause_all_diag_count = 0;
static uint32_t pause_all_diag_last_duration_ms = 0;
static uint32_t pause_all_diag_max_duration_ms = 0;

static inline void serviceBwcLoopDiag()
{
    uint32_t now = millis();
    if (bwc_diag_last_loop_ms != 0) {
        uint32_t gap = (uint32_t)(now - bwc_diag_last_loop_ms);
        bwc_diag_last_gap_ms = gap;
        if (gap > bwc_diag_max_gap_ms) bwc_diag_max_gap_ms = gap;
        if (gap > 250)  bwc_diag_gap_over_250++;
        if (gap > 500)  bwc_diag_gap_over_500++;
        if (gap > 1000) bwc_diag_gap_over_1000++;
        if (gap > 3000) bwc_diag_gap_over_3000++;
    }
    bwc_diag_last_loop_ms = now;
}

                                                                               
                                                                             
                                                                               
extern "C" void sar_bearssl_service_hook(void)
{
#if defined(ESP8266)
    if (!sar_cloud_tls_handshake_in_progress || !bwc) return;
    const uint32_t now = millis();
    if (sar_tls_pump_service_last_ms != 0) {
        const uint32_t gap = (uint32_t)(now - sar_tls_pump_service_last_ms);
        if (gap < 25UL) return;
        if (gap > sar_tls_pump_service_max_gap_ms) sar_tls_pump_service_max_gap_ms = gap;
    }
    sar_tls_pump_service_last_ms = now;
    bwc->loop();
    serviceBwcLoopDiag();
    sar_tls_pump_service_count++;
#endif
}

                                                                                  
                                                                                  
void pause_cloud_tasks_only(bool action)
{
    if(action)
    {
        if(periodicTimer.active()) periodicTimer.detach();
        if(startComplete_ticker.active()) startComplete_ticker.detach();
        if(updateWSTimer.active()) updateWSTimer.detach();
        if(bootlogTimer.active()) bootlogTimer.detach();
        if(ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    } else
    {
        periodicTimer.attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
        startComplete_ticker.attach(60, []{ if(useMqtt) enableMqtt = true; startComplete_ticker.detach(); });
        if (SAR_LOCAL_WEBSOCKET_ENABLED) updateWSTimer.attach(2.0, []{ sendWSFlag = true; });
    }
}

static inline bool isPaired()
{
    String code = mqttPairingCode;
    code.trim();
    return code.length() >= 4;                                          
}

static inline bool cloudPollingEnabled()
{
                                                                                        
    return mqttCloudMode && useMqtt && (WiFi.status() == WL_CONNECTED);
}

static inline uint32_t presenceLastOkAgeMs(uint32_t now)
{
    if (presence_last_ok_ms == 0) return 0xFFFFFFFFUL;
    return (uint32_t)(now - presence_last_ok_ms);
}

static inline bool presenceFreshForMqtt(uint32_t now)
{
    if (!presence_allowed) return false;
    if (presence_last_ok_ms == 0) return false;
    return (presenceLastOkAgeMs(now) <= PRESENCE_MQTT_FRESH_MAX_AGE_MS);
}

static inline uint32_t presenceCostGuardRemainingMs(uint32_t now);

static inline bool presenceMqttShouldRunNow(uint32_t now)
{
    const bool bootstrapActive = cloudBootstrapActive();
    const bool presenceFresh = presenceFreshForMqtt(now);
    const bool costGuardBlocking = presence_cost_guard_active && (presenceCostGuardRemainingMs(now) > 0);

                                                                     
                                                                      
                                                       
    const bool mqttEnabledForMode = mqttCloudMode ? useMqtt : (useMqtt && enableMqtt);

    return mqttEnabledForMode && !costGuardBlocking && ((presenceFresh && presence_allowed) || bootstrapActive);
}

static inline uint32_t presenceStickyIpRemainingMs(uint32_t now)
{
    if (presence_ip_preferred_until_ms == 0) return 0;
    if ((int32_t)(presence_ip_preferred_until_ms - now) <= 0) return 0;
    return (uint32_t)(presence_ip_preferred_until_ms - now);
}

static inline uint32_t presenceCostGuardRemainingMs(uint32_t now)
{
    if (!presence_cost_guard_active) return 0;
    if ((int32_t)(presence_cost_guard_until_ms - now) <= 0) return 0;
    return (uint32_t)(presence_cost_guard_until_ms - now);
}





                                                         
                                                                                              
static bool cloudPresenceFetch(bool &outAllowed)
{
#if !defined(ESP8266)
    (void)outAllowed;
    return false;
#else
    initPresenceTlsOnce();

    presence_poll_seq++;
    presence_last_poll_start_ms = millis();
    presence_last_poll_duration_ms = 0;
    presence_last_dns_duration_ms = 0;
    presence_last_ntp_wait_ms = 0;
    presence_last_tls_connect_ms = 0;
    presence_last_http_send_ms = 0;
    presence_last_wait_first_byte_ms = 0;
    presence_last_status_read_ms = 0;
    presence_last_header_read_ms = 0;
    presence_last_body_read_ms = 0;
    presence_last_http_duration_ms = 0;
    presence_last_bytes_read = 0;
    presence_last_content_len = -1;
    presence_last_dns_tries = 0;
    presence_last_tls_mode = 0;
    presence_last_outcome = 0;
    presence_last_heap_start = ESP.getFreeHeap();
    presence_last_block_start = ESP.getMaxFreeBlockSize();
    presence_last_frag_start = ESP.getHeapFragmentation();
    presence_last_heap_min = presence_last_heap_start;
    presence_last_block_min = presence_last_block_start;
    presenceSetStage("start", true);

    presence_last_dns_ok = false;
    presence_last_connect_ok = false;
    presence_last_ip_fallback_used = false;
    presence_last_force_ip_mode = ((int32_t)(presence_ip_preferred_until_ms - millis()) > 0);
    presence_last_ssl_err = 0;
    presence_last_body_byte = -1;
    presence_last_http_status_line = "";
    appendPresenceDiag(String(F("poll start wifi=")) + ((WiFi.status() == WL_CONNECTED) ? F("1") : F("0")) +
                       F(" ip=") + WiFi.localIP().toString() +
                       F(" gw=") + WiFi.gatewayIP().toString() +
                       F(" dns1=") + WiFi.dnsIP(0).toString() +
                       F(" dns2=") + WiFi.dnsIP(1).toString() +
                       F(" rssi=") + String(WiFi.RSSI()) +
                       F(" heap=") + String(presence_last_heap_start) +
                       F(" block=") + String(presence_last_block_start) +
                       F(" frag=") + String(presence_last_frag_start));

    if (PRESENCE_DEBUG) {
      Serial.printf_P(PSTR("PRESENCE NET: ip=%s gw=%s dns1=%s dns2=%s rssi=%d\n"),
                      WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.dnsIP(0).toString().c_str(),
                      WiFi.dnsIP(1).toString().c_str(),
                      WiFi.RSSI());
    }

                                           
                                                                                
                                                                         
                                                                       
                                                                        
                                                                             
    struct _PresenceHardFreezeGuard {
      _PresenceHardFreezeGuard() {
        if (bwc) bwc->beginCloudPollingGuard(20000UL);
        pause_all(true);
      }
      ~_PresenceHardFreezeGuard() {
        pause_all(false);
        if (bwc) bwc->finishCloudPollingGuard(3000UL);
      }
    } _presenceHardFreezeGuard;

    auto pumpBackground = [&]() {
                                                                          
                                                                      
                                                                          
      delay(0);
      yield();
    };

    auto logTlsError = [&](const char* prefix) {
      int sslErr = presenceClient.getLastSSLError();
      presence_last_ssl_err = sslErr;
      char err[128];
      presenceClient.getLastSSLError(err, sizeof(err));
      appendPresenceDiag(String(prefix) + String(" code=") + String(sslErr) + String(" msg=") + String(err), true);
      if (PRESENCE_DEBUG) {
        Serial.printf_P(PSTR("%s code=%d msg=%s\n"), prefix, sslErr, err);
      }
    };

    auto safeReadLine = [&](String &out, uint32_t timeoutMs) -> bool {
      uint32_t t0 = millis();
      while ((millis() - t0) < timeoutMs) {
        if (presenceClient.available()) {
          out = presenceClient.readStringUntil('\n');
          out.trim();
          return true;
        }
        if (!presenceClient.connected()) return false;
        pumpBackground();
      }
      return false;
    };

                                                                          
    uint32_t fetchGateFreeHeap = ESP.getFreeHeap();
    uint32_t fetchGateMaxBlock = ESP.getMaxFreeBlockSize();
    if (fetchGateFreeHeap < PRESENCE_HEAP_SKIP_FREE_MS || fetchGateMaxBlock < presence_heap_skip_block_min_current) {
      if (PRESENCE_DEBUG) {
        Serial.printf_P(PSTR("PRESENCE SKIP: low heap fh=%u mb=%u blockMin=%u\n"),
                        fetchGateFreeHeap, fetchGateMaxBlock, presence_heap_skip_block_min_current);
      }
      presence_low_heap_skip_count++;
      presence_last_skip_heap_free = fetchGateFreeHeap;
      presence_last_skip_heap_block = fetchGateMaxBlock;
      presence_last_outcome = 7;
      presenceSetStage("low_heap", true);
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(String(F("skip low heap fh=")) + fetchGateFreeHeap +
                         F(" mb=") + fetchGateMaxBlock +
                         F(" blockMin=") + presence_heap_skip_block_min_current, true);
      return false;
    }

    String devId = getMacClean();
    String urlPath = String(PRESENCE_PATH_PREFIX) + devId;

    IPAddress resolvedIp;
    bool dnsOk = false;
    presenceSetStage("dns", true);
    uint32_t t_dns_start = millis();
    for (uint8_t dnsTry = 0; dnsTry < 3 && !dnsOk; ++dnsTry) {
      presence_last_dns_tries = dnsTry + 1;
      dnsOk = WiFi.hostByName(PRESENCE_HOST, resolvedIp);
      if (!dnsOk) {
        uint32_t dnsWaitStart = millis();
        while ((uint32_t)(millis() - dnsWaitStart) < 150UL) {
          pumpBackground();
        }
      }
    }
    presence_last_dns_duration_ms = millis() - t_dns_start;
    presence_last_dns_ok = dnsOk;
    presence_last_resolved_ip = resolvedIp;
    if (PRESENCE_DEBUG) {
      if (dnsOk) {
        Serial.printf_P(PSTR("PRESENCE DNS: %s -> %s\n"),
                        PRESENCE_HOST, resolvedIp.toString().c_str());
      } else {
        Serial.printf_P(PSTR("PRESENCE DNS FAIL: %s\n"), PRESENCE_HOST);
      }
      Serial.printf_P(PSTR("PRESENCE REQ: https://%s%s (deviceId=%s)\n"),
                      PRESENCE_HOST, urlPath.c_str(), devId.c_str());
    }
    appendPresenceDiag(String(F("dns ")) + (dnsOk ? F("ok ") : F("fail ")) + resolvedIp.toString() +
                       F(" tries=") + String(presence_last_dns_tries) +
                       F(" ms=") + String(presence_last_dns_duration_ms));
    if (!dnsOk) {
      presence_last_outcome = 2;
      presenceSetStage("dns_fail", true);
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(F("abort after dns fail"), true);
      return false;
    }

                                                
    if (presenceClient.connected()) {
      presenceClient.stop();
      yield();
    }

    presenceSetStage("ntp", true);
    uint32_t t_ntp_start = millis();
    waitForValidTime(2000);
    presence_last_ntp_wait_ms = millis() - t_ntp_start;
    presenceClient.setX509Time(time(nullptr));

    bool preferIpMode = ((int32_t)(presence_ip_preferred_until_ms - millis()) > 0);
    bool connected = false;

    if (!preferIpMode) {
      presenceSetStage("tls_host", true);
      uint32_t t_tls_start = millis();
      connected = presenceClient.connect(PRESENCE_HOST, PRESENCE_PORT);
      presence_last_tls_connect_ms = millis() - t_tls_start;
      presence_last_tls_mode = 1;
      if (!connected) {
        logTlsError("PRESENCE TLS connect FAIL(host):");
        if (presence_last_ssl_err == 62) {
          if (presence_host_anchor_fail_streak < 255) presence_host_anchor_fail_streak++;
          presence_ip_preferred_until_ms = millis() + PRESENCE_FORCE_IP_WINDOW_MS;
          appendPresenceDiag(String(F("host trust-anchor fail -> prefer ip for ms=")) + String(PRESENCE_FORCE_IP_WINDOW_MS), true);
        }
      } else {
        presence_host_anchor_fail_streak = 0;
        presence_ip_preferred_until_ms = 0;
      }
    } else {
      appendPresenceDiag(F("host tls skipped -> sticky ip mode active"));
    }

    if (!connected && resolvedIp != IPAddress((uint32_t)0)) {
      presenceClient.stop();
      yield();
      appendPresenceDiag(String(F("fallback ip connect ")) + resolvedIp.toString());
      presenceSetStage("tls_ip", true);
      uint32_t t_tls_ip_start = millis();
      connected = presenceClient.connect(resolvedIp, PRESENCE_PORT);
      presence_last_tls_connect_ms = millis() - t_tls_ip_start;
      presence_last_tls_mode = 2;
      if (connected) {
        presence_last_ip_fallback_used = true;
        presence_ip_preferred_until_ms = millis() + PRESENCE_FORCE_IP_WINDOW_MS;
        appendPresenceDiag(F("fallback ip connect ok"), true);
      } else {
        logTlsError("PRESENCE TLS connect FAIL(ip):");
                                                                           
        if (preferIpMode) {
          presence_ip_preferred_until_ms = 0;
          appendPresenceDiag(F("sticky ip mode cleared after ip connect fail"), true);
        }
      }
    }
    presence_last_connect_ok = connected;
    if (!connected) {
      if (PRESENCE_DEBUG) {
        Serial.printf_P(PSTR("PRESENCE TLS FAIL NET: dns1=%s dns2=%s ip=%s gw=%s\n"),
                        WiFi.dnsIP(0).toString().c_str(),
                        WiFi.dnsIP(1).toString().c_str(),
                        WiFi.localIP().toString().c_str(),
                        WiFi.gatewayIP().toString().c_str());
      }
      presence_last_outcome = 3;
      presenceSetStage("tls_fail", true);
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(String(F("abort after tls connect fail tlsMs=")) + String(presence_last_tls_connect_ms), true);
      return false;
    }

    if (PRESENCE_DEBUG) {
      Serial.printf_P(PSTR("PRESENCE TLS connected: mfln=%d rx=%u tx=%u\n"),
                      presenceClient.getMFLNStatus() ? 1 : 0,
                      presenceRxBuf,
                      presenceTxBuf);
    }
    appendPresenceDiag(String(F("tls connected fallback=")) + (presence_last_ip_fallback_used ? F("1") : F("0")) +
                       F(" stickyIp=") + (((int32_t)(presence_ip_preferred_until_ms - millis()) > 0) ? F("1") : F("0")) +
                       F(" mfln=") + (presenceClient.getMFLNStatus() ? F("1") : F("0")) +
                       F(" tlsMs=") + String(presence_last_tls_connect_ms) +
                       F(" heapMin=") + String(presence_last_heap_min) +
                       F(" blockMin=") + String(presence_last_block_min));

                                  
    presenceSetStage("http_send", true);
    uint32_t t_http_start = millis();
    uint32_t t_http_send_start = t_http_start;
    presenceClient.print(
      String("GET ") + urlPath + " HTTP/1.0\r\n" +
      "Host: " + String(PRESENCE_HOST) + "\r\n" +
      "User-Agent: esp8266\r\n" +
      "Accept: text/plain\r\n" +
      "\r\n"
    );
    presenceClient.flush();
    presence_last_http_send_ms = millis() - t_http_send_start;

                                        
    presenceSetStage("wait_byte", true);
    uint32_t t_wait = millis();
    while (presenceClient.connected() && !presenceClient.available() &&
           (millis() - t_wait) < PRESENCE_WAIT_FIRST_BYTE_MS) {
      pumpBackground();
    }

    presence_last_wait_first_byte_ms = millis() - t_wait;

    String statusLine;
    presenceSetStage("status", true);
    uint32_t t_status_start = millis();
    safeReadLine(statusLine, PRESENCE_WAIT_STATUS_MS);
    presence_last_status_read_ms = millis() - t_status_start;
    presence_last_http_status_line = statusLine;
    if (PRESENCE_DEBUG) {
      Serial.printf_P(PSTR("PRESENCE HTTP status: '%s' avail=%d connected=%d\n"),
                      statusLine.c_str(),
                      (int)presenceClient.available(),
                      (int)presenceClient.connected());
    }

    if (statusLine.length() == 0) {
      logTlsError("PRESENCE FAIL: empty statusline");
      presence_last_outcome = 4;
      presenceSetStage("empty_status", true);
      presence_last_http_duration_ms = millis() - t_http_start;
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(String(F("empty statusline waitByteMs=")) + String(presence_last_wait_first_byte_ms) +
                         F(" statusMs=") + String(presence_last_status_read_ms), true);
      appendPresenceDiag(String(F("http non-2xx status=")) + statusLine, true);
      presenceClient.stop();
      presence_conn_open_ms = 0;
      presence_last_use_ms  = 0;
      return false;
    }

    if (!(statusLine.startsWith("HTTP/1.1 2") || statusLine.startsWith("HTTP/1.0 2")))
    {
      if (PRESENCE_DEBUG) {
        for (int i = 0; i < 12; i++)
        {
          String line;
          if (!safeReadLine(line, 500)) break;
          if (line.length() == 0) break;
          Serial.printf_P(PSTR("PRESENCE HDR: %s\n"), line.c_str());
          pumpBackground();
        }
      }

      presence_last_outcome = 5;
      presenceSetStage("http_non2xx", true);
      presence_last_http_duration_ms = millis() - t_http_start;
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(String(F("http non-2xx status=")) + statusLine +
                         F(" httpMs=") + String(presence_last_http_duration_ms), true);
      presenceClient.stop();
      presence_conn_open_ms = 0;
      presence_last_use_ms  = 0;
      return false;
    }

    int contentLen = -1;
    presenceSetStage("headers", true);
    uint32_t t_hdr = millis();
    while ((millis() - t_hdr) < PRESENCE_WAIT_HEADER_MS)
    {
      if (!presenceClient.available())
      {
        if (!presenceClient.connected()) break;
        pumpBackground();
        continue;
      }

      String line = presenceClient.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;

      if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
      {
        String v = line.substring(strlen("Content-Length:"));
        v.trim();
        contentLen = v.toInt();
      }

      pumpBackground();
    }

    presence_last_header_read_ms = millis() - t_hdr;
    presence_last_content_len = contentLen;

    int firstNonWs = -1;
    int bytesRead = 0;
    presenceSetStage("body", true);
    uint32_t tBody = millis();
    while ((millis() - tBody) < PRESENCE_WAIT_BODY_MS)
    {
      while (presenceClient.available())
      {
        int b = presenceClient.read();
        bytesRead++;

        if (firstNonWs < 0 && b >= 0 && b != '\n' && b != '\r' && b != ' ' && b != '\t')
        {
          firstNonWs = b;
        }

        if (contentLen >= 0 && bytesRead >= contentLen)
          goto body_done;
      }

      if (!presenceClient.connected())
        break;

      pumpBackground();
    }

body_done:

    presence_last_body_read_ms = millis() - tBody;
    presence_last_bytes_read = bytesRead;
    presence_last_http_duration_ms = millis() - t_http_start;

    if (firstNonWs < 0)
    {
      if (PRESENCE_DEBUG) Serial.println(F("PRESENCE BODY: <no non-ws byte>"));
      presence_last_outcome = 6;
      presenceSetStage("empty_body", true);
      presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
      appendPresenceDiag(String(F("body no non-ws byte contentLen=")) + String(contentLen) +
                         F(" bytes=") + String(bytesRead) +
                         F(" bodyMs=") + String(presence_last_body_read_ms), true);
      presenceClient.stop();
      presence_conn_open_ms = 0;
      presence_last_use_ms  = 0;
      return false;
    }

    if (PRESENCE_DEBUG)
    {
      Serial.printf_P(PSTR("PRESENCE BODY first byte: '%c' (%d) contentLen=%d bytesRead=%d\n"),
                      (char)firstNonWs, firstNonWs, contentLen, bytesRead);
    }

    presence_last_body_byte = firstNonWs;
    outAllowed = (firstNonWs == '1');
                                                                                                              
    presence_last_ok_ms = millis();
    presence_last_allowed_value = outAllowed;
    presence_last_dns_ok = true;
    presence_last_connect_ok = true;
    presence_last_outcome = 1;
    presenceSetStage("ok", true);
    presence_last_poll_duration_ms = millis() - presence_last_poll_start_ms;
    presence_last_heap_end = ESP.getFreeHeap();
    presence_last_block_end = ESP.getMaxFreeBlockSize();
    presence_last_frag_end = ESP.getHeapFragmentation();
    appendPresenceDiag(String(F("result allowed=")) + (outAllowed ? F("1") : F("0")) +
                       F(" bodyByte=") + String((char)firstNonWs) +
                       F(" totalMs=") + String(presence_last_poll_duration_ms) +
                       F(" dnsMs=") + String(presence_last_dns_duration_ms) +
                       F(" ntpMs=") + String(presence_last_ntp_wait_ms) +
                       F(" tlsMs=") + String(presence_last_tls_connect_ms) +
                       F(" httpMs=") + String(presence_last_http_duration_ms) +
                       F(" heapEnd=") + String(presence_last_heap_end) +
                       F(" blockEnd=") + String(presence_last_block_end), true);

                                                                 
    presenceClient.stop();
    presence_conn_open_ms = 0;
    presence_last_use_ms  = 0;

    return true;
#endif
}


                                                       
static void dbgCloudState(const char* tag)
{
#if defined(ESP8266)
  if (!mqttCloudMode) return;

  Serial.printf_P(PSTR("[%s] presAllowed=%d paused=%d nextPollIn=%ld activeIn=%ld graceIn=%ld "
                       "mqttConn=%d mqttState=%d enableMqtt=%d paired=%d codeLen=%u heap=%u\n"),
    tag,
    presence_allowed ? 1 : 0,
    presence_polling_paused ? 1 : 0,
    (long)(presence_next_poll_ms - millis()),
    (long)(presence_active_until_ms - millis()),
    (long)(presence_grace_until_ms - millis()),
    (mqttClient ? mqttClient->connected() : 0),
    (mqttClient ? mqttClient->state() : 999),
    enableMqtt ? 1 : 0,
    isPaired() ? 1 : 0,
    (unsigned)mqttPairingCode.length(),
    ESP.getFreeHeap()
  );
#endif
}



static void resetCloudPresenceTransport(const char* reason)
{
#if defined(ESP8266)
    if (mqttClient && mqttClient->connected()) {
        publishStatusRetained("Asleep");
    }
    if (mqttCloudMode) cloudTlsStopBounded(true, "presence-transport-reset");
    else {
        if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
        if (aWifiClient) aWifiClient->stop();
    }
    presenceClient.stop();
                                                      
    presenceTlsReady = false;
    presenceMflnProbed = false;
    presenceMflnOk = false;
#endif
    presence_last_dns_ok = false;
    presence_last_connect_ok = false;
    presence_last_resolved_ip = IPAddress((uint32_t)0);
    presence_last_http_status_line = "";
    presence_last_body_byte = -1;
    presence_last_ssl_err = 0;
    presence_last_ip_fallback_used = false;
    presence_last_force_ip_mode = false;
    presence_host_anchor_fail_streak = 0;
    presence_ip_preferred_until_ms = 0;
    if (reason) {
        appendPresenceDiag(String(F("cloud transport reset: ")) + String(reason), true);
    }
}

static void updatePresenceGate()
{
  uint32_t now = millis();
  presence_last_attempt_ms = now;

  if (!cloudPollingEnabled())
  {
                                                                                                    
                                                                                                  
    presence_allowed = false;
    presence_active_until_ms = 0;
    presence_grace_until_ms  = 0;
    if (presence_next_poll_ms == 0) {
      presence_next_poll_ms = now + 5000UL;
    } else {
      presence_next_poll_ms = now + 5000UL;
    }
    if (PRESENCE_DEBUG) dbgCloudState("POLLING_OFF_RETRY");
    return;
  }

                         
  if (presence_next_poll_ms != 0 && (int32_t)(now - presence_next_poll_ms) < 0) {
    return;
  }

#if defined(ESP8266)
                                                                  
                                                               
                                                                              
                                                                                  
  uint32_t gateFreeHeap = ESP.getFreeHeap();
  uint32_t gateMaxBlock = ESP.getMaxFreeBlockSize();
  bool gateFreeLow = (gateFreeHeap < PRESENCE_HEAP_SKIP_FREE_MS);
  bool gateBlockLow = (gateMaxBlock < presence_heap_skip_block_min_current);
  if (gateFreeLow || gateBlockLow) {
    presence_low_heap_skip_count++;
    presence_last_skip_heap_free = gateFreeHeap;
    presence_last_skip_heap_block = gateMaxBlock;

                                                                                   
                                                                                  
                                                                        
    if (!gateFreeLow &&
        gateMaxBlock >= PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS &&
        gateMaxBlock + 300UL >= presence_heap_skip_block_min_current) {
      presence_adaptive_near_skip_count++;
      if (presence_adaptive_near_skip_count >= 3) {
        presenceAdaptiveLowerBlockMin(gateMaxBlock, F("near_skips"));
      }
    } else {
      presence_adaptive_near_skip_count = 0;
    }

                                                                                  
                                                               
                                                           
    if (!gateFreeLow && gateBlockLow &&
        presence_heap_skip_block_min_current > PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS) {
      uint32_t ageSinceOk = (presence_last_ok_ms != 0) ? (uint32_t)(now - presence_last_ok_ms) : now;
      if (ageSinceOk > PRESENCE_HEAP_SKIP_STALL_REARM_MS) {
        presenceAdaptiveLowerBlockMin(PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS, F("stall_rearm"));
      }
    }

    appendPresenceDiag(String(F("gate skip low heap fh=")) + presence_last_skip_heap_free +
                       F(" mb=") + presence_last_skip_heap_block +
                       F(" blockMin=") + presence_heap_skip_block_min_current, true);

                                                                        
                                                                                                  
    presence_next_poll_ms = now + addJitter(45000UL);
    return;
  }

  presence_adaptive_last_start_heap = gateFreeHeap;
  presence_adaptive_last_start_block = gateMaxBlock;
#endif

  bool allowed = false;
  bool ok = cloudPresenceFetch(allowed);

#if defined(ESP8266)
                                                                            
                                                                              
                                                      
  if (ok) {
    if (presence_adaptive_last_start_heap >= PRESENCE_HEAP_SKIP_FREE_MS &&
        presence_adaptive_last_start_block >= PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS &&
        presence_adaptive_last_start_block <= presence_heap_skip_block_min_current + 300UL) {
      presence_adaptive_success_count++;
      if (presence_adaptive_success_count >= 5 &&
          presence_heap_skip_block_min_current > PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS) {
        presenceAdaptiveLowerBlockMin(presence_adaptive_last_start_block, F("success"));
      }
    } else {
      presence_adaptive_success_count = 0;
    }
  } else {
    presence_adaptive_success_count = 0;
    if (presence_last_outcome == 3) {
      presenceAdaptiveRaiseBlockMin(presence_adaptive_last_start_block, F("tls_fail"));
    }
  }
#endif

  if (PRESENCE_DEBUG) {
    Serial.printf_P(PSTR("PRESENCE > ok=%d allowed=%d heap=%u\n"),
                    ok ? 1 : 0, allowed ? 1 : 0, ESP.getFreeHeap());
  }

  if (ok) {
    presence_last_ok_ms = now;
    presence_stall_count = 0;
    presence_cost_guard_active = false;
    presence_cost_guard_until_ms = 0;
    presence_last_stall_rearm_ms = 0;

    presence_allowed = allowed;
    presence_last_allowed_value = allowed;

                                                         
    if (allowed) {
      presence_active_until_ms = now + PRESENCE_ACTIVE_WINDOW_MS;
      presence_grace_until_ms  = now + PRESENCE_GRACE_MS;
      presence_next_poll_ms = now + addJitter(PRESENCE_POLL_ONLINE_MS);



    } else {
      presence_active_until_ms = 0;
      presence_grace_until_ms  = 0;
      presence_next_poll_ms    = now + addJitter(PRESENCE_POLL_OFFLINE_MS);
    }
    return;
  }

                                                          
                                                             
  if (++presence_stall_count >= 3) {
    resetCloudPresenceTransport("3 consecutive fetch failures");
    presence_stall_count = 0;
    presence_next_poll_ms = now + 5000UL;
  } else {
    presence_next_poll_ms = now + addJitter(PRESENCE_POLL_BURST_MS);
  }

}



#if defined(ESP8266)
static String sha256Hex(const String& input)
{
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, input.c_str(), input.length());

    unsigned char out[32];
    br_sha256_out(&ctx, out);

    static const char hex[] = "0123456789abcdef";
    char buf[65];
    for (int i = 0; i < 32; i++)
    {
        buf[i * 2]     = hex[(out[i] >> 4) & 0x0F];
        buf[i * 2 + 1] = hex[out[i] & 0x0F];
    }
    buf[64] = 0;
    return String(buf);
}
#endif


static String lastPairHash;

static void sarPrepareFreshV2PairingAfterMigration()
{
                                                                              
                                                                                    
    mqttPairingSentHash = "";
    lastPairHash = "";
    saveMqtt();
}


static void publishPairingHash()
{
    if (!mqttClient || !mqttClient->connected()) return;
    if (!mqttCloudMode) return;

    String code = mqttPairingCode;
    code.trim();
    if (code.length() < 4) return;

    String mac = getMacClean();
    String payload = mac + ":" + code;

#if defined(ESP8266)
    String hash = sha256Hex(payload);
#else
    String hash = "";
#endif

    
                                                                                                   
                                                                                                         
    if (hash.length() == 0) return;

                                                  
    if (hash == mqttPairingSentHash) {
        lastPairHash = hash;                      
        return;
    }

                                                           
    if (hash == lastPairHash) return;

    String topic = String(mqttBaseTopic) + "/pairing/hash";

    bool ok = mqttClient->publish(topic.c_str(), hash.c_str(), true);
    if (!ok) {
        Serial.println(F("PAIRING > publish failed"));
        return;
    }

                                          
    lastPairHash = hash;
    mqttPairingSentHash = hash;
    saveMqtt();

    Serial.print(F("PAIRING > published NEW retained hash to "));
    Serial.println(topic);
}
static void publishStatusRetained(const char* status)
{
    if (!mqttClient || !mqttClient->connected()) return;

    mqttClient->publish((String(mqttBaseTopic) + F("/Status")).c_str(), status, true);

                                                                               
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 150) {
        if (mqttClient) mqttClient->loop();
        delay(0);                      
    }
}


#if defined(ESP8266)
static void hardResetMqttStack(const char* reason)
{
    Serial.printf_P(PSTR("MQTT HARD RESET: %s\n"), reason ? reason : "(no reason)");

                                                                

                                                                            
    if (mqttCloudMode) {
        cloudTlsStopBounded(true, "mqtt-hard-reset");
    } else {
        if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
        if (aWifiClient) aWifiClient->stop();
    }
                                                 
    tlsClient   = &tlsClientStatic;
    tlsCa       = &mqttCloudCaRootYeStatic;
    aWifiClient = tlsClient;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);


                                                         
    presenceClient.stop();

                                                                              
                                                                                 
                                                                          
    if (mqttClient) mqttClient->setClient(*aWifiClient);

                                               
    cloud_next_mqtt_try_ms = millis() + 5000UL;
    mqtt_fail_streak = 0;
    mqtt_stack_reset_ms = millis();
}

static void cloudMqttSupervisorTick()
{
    if (!mqttCloudMode) return;
    if (!enableMqtt) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (SAR_CLOUD_V2_ALWAYS_ON) {
        if (!cloudV2CredentialsProvisioned()) return;
    } else {
        if (!isPaired()) return;
        const bool shouldRun = presence_allowed || cloudBootstrapActive();
        if (!shouldRun) return;
    }

    const uint32_t now = millis();

                                                                      
    if (!mqttClient) {
                            
        if ((int32_t)(now - mqtt_stack_reset_ms) > 30000) {
            hardResetMqttStack("mqttClient==nullptr");
        }
        return;
    }

    if (mqttClient->connected()) {
        mqtt_last_connected_ms = now;
        mqtt_fail_streak = 0;
        return;
    }

                                                                                             
    const bool tooLongOffline = (mqtt_last_connected_ms != 0) && ((uint32_t)(now - mqtt_last_connected_ms) > 10UL*60UL*1000UL);
    const bool manyFails      = (mqtt_fail_streak >= 12);                                               

                             
    if ((tooLongOffline || manyFails) && ((uint32_t)(now - mqtt_stack_reset_ms) > 120000UL)) {
        hardResetMqttStack(tooLongOffline ? "offline>10min" : "many connect fails");
    }
}
#endif

static void cloudV2MqttTick(bool newData)
{
    if (!mqttClient) return;
#if defined(ESP8266)
    const bool mqttWasConnected = mqttClient->connected();
    const bool mqttLoopOk = mqttClient->loop();
    const bool mqttIsConnectedAfterLoop = mqttClient->connected();
    if (mqttWasConnected && (!mqttLoopOk || !mqttIsConnectedAfterLoop)) {
        mqtt_loop_drop_count++;
        mqtt_last_loop_drop_ms = millis();
        mqtt_last_loop_drop_state = mqttClient->state();

                                                                           
                                                                           
                                                                        
        cloudTlsStopBounded(false, "mqtt-loop-drop");
        cloud_next_mqtt_try_ms = millis() + 5000UL;
    }
#else
    mqttClient->loop();
#endif

    if (!enableMqtt || !cloudV2CredentialsProvisioned()) {
        if (mqttClient->connected()) publishStatusRetained("Asleep");
        cloudTlsStopBounded(true, "cloud-disabled-or-unprovisioned");
        return;
    }

    if (!mqttClient->connected()) {
        const uint32_t nowTry = millis();
        if (cloud_next_mqtt_try_ms == 0 || (int32_t)(nowTry - cloud_next_mqtt_try_ms) >= 0) {
            cloud_next_mqtt_try_ms = nowTry + 5000UL;
#if defined(ESP8266)
            mqtt_last_connect_trigger = "cloudV2Tick:not-connected";
#endif
            Serial.println(F("[CloudV2] mqttConnect()"));
            mqttConnect();
        }
    }
    if (!mqttClient->connected()) return;

                                                                        
                                                                             
    cloudV2ResponseTick();

    uint32_t nowMs = millis();
    uint32_t intervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
    if (intervalMs > 0 && (int32_t)(nowMs - sar_cloud_next_telemetry_ms) >= 0) {
        sar_cloud_next_telemetry_ms = nowMs + intervalMs;
        sendMQTTFlag = true;
    }

    String msg; msg.reserve(32);
    bwc->getButtonName(msg);
    if (!msg.equals(prevButtonName)) {
        if (mqttPublishChecked(String(mqttBaseTopic) + F("/button"), msg, false)) prevButtonName = msg;
    }

    if (newData || sendMQTTFlag) { sendMQTT(); sendMQTTFlag = false; }
    if (send_mqtt_cfg_needed) {
        send_mqtt_cfg_needed = false;
        mqttServiceTick(); sendMQTTConfig(); mqttServiceTick();
    }
}


                
char *stack_start;
uint32_t heap_water_mark;

                                                                   
                                                            
                                                                
static OneWire oneWireStatic(231);
static DallasTemperature tempSensorsStatic(&oneWireStatic);
OneWire *oneWire = &oneWireStatic;
DallasTemperature *tempSensors = &tempSensorsStatic;

WiFiEventHandler gotIpEventHandler, disconnectedEventHandler;
void cb_gotIP(const WiFiEventStationModeGotIP& event)
{
#if defined(ESP8266)
    wifi_got_ip_count++;
    wifi_last_got_ip_ms = millis();
    cloud_dns_preflight_valid_until_ms = 0;
    tls_admission_blocked_since_ms = 0;
    got_ip_heap_before = ESP.getFreeHeap();
    got_ip_block_before = ESP.getMaxFreeBlockSize();
#endif
    Serial.print("got IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf_P(PSTR("[WiFi] gateway=%s dns1=%s dns2=%s rssi=%d\n"),
                    WiFi.gatewayIP().toString().c_str(),
                    WiFi.dnsIP(0).toString().c_str(),
                    WiFi.dnsIP(1).toString().c_str(),
                    WiFi.RSSI());

                                                                       
                                                                              
    if (mqttCloudMode) {
                                                             
        cloud_next_mqtt_try_ms = millis() + 5000UL;
        mqtt_telemetry_enabled = false;
        mqtt_next_telemetry_ms = 0;
        sar_cloud_next_telemetry_ms = 0;
#if defined(ESP8266)
        cloudTlsStopBounded(true, "wifi-got-ip");
#endif
        if (SAR_CLOUD_V2_ALWAYS_ON) {
            Serial.println(F("[WiFi] Cloud V2 MQTT re-armed after 5s IP settling"));
        } else {
            presence_allowed = false;
            presence_next_poll_ms = 0;
            presence_active_until_ms = 0;
            presence_grace_until_ms  = 0;
            presence_polling_paused  = false;
            cloud_pair_bootstrap_until_ms = 0;
#if defined(ESP8266)
            presenceClient.stop();
#endif
        }
    } else {
                                                                                
                                                                                
        custom_mqtt_kick = (enableMqtt || useMqtt);
        custom_mqtt_kick_at_ms = millis();
    }

    startNTP();
    startOTA();
    startHttpServer();                                             
                                                                                                  
    if (SAR_LOCAL_WEBSOCKET_ENABLED) startWebSocket();

#if defined(ESP8266)
    got_ip_heap_after = ESP.getFreeHeap();
    got_ip_block_after = ESP.getMaxFreeBlockSize();
    got_ip_heap_delta = (int32_t)got_ip_heap_after - (int32_t)got_ip_heap_before;
    got_ip_block_delta = (int32_t)got_ip_block_after - (int32_t)got_ip_block_before;
    Serial.printf_P(PSTR("[WiFi] GotIP diag heap %u->%u (%ld), block %u->%u (%ld)\n"),
                    got_ip_heap_before, got_ip_heap_after, (long)got_ip_heap_delta,
                    got_ip_block_before, got_ip_block_after, (long)got_ip_block_delta);
#endif
}

void cb_disconnected(const WiFiEventStationModeDisconnected& event)
{
#if defined(ESP8266)
    wifi_disconnect_count++;
    wifi_last_disconnect_ms = millis();
    wifi_last_disconnect_reason = event.reason;
    cloud_dns_preflight_valid_until_ms = 0;
    tls_admission_blocked_since_ms = 0;
#endif
    Serial.println(F("disconnected"));

                                                                        
    presence_allowed = false;
    presence_next_poll_ms = 0;
    presence_active_until_ms = 0;
    presence_grace_until_ms  = 0;
    cloud_next_mqtt_try_ms = 0;

#if defined(ESP8266)
                                                                         
                                                                                
    if (mqttCloudMode) cloudTlsStopBounded(false, "wifi-disconnected");
    else {
        if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
        if (aWifiClient) aWifiClient->stop();
    }
    presenceClient.stop();
#endif
    resetCustomHaDiscoverySchedule();
}


void setup()
{
    
                           
    char stack;
    stack_start = &stack;

    Serial.begin(115200);
    randomSeed(ESP.getChipId() ^ micros());
#if defined(ESP8266)
    generateBootId();
#endif
    Serial.println();
    Serial.println(F("[BOOT] ResetInfo:"));
    Serial.println(ESP.getResetInfo());

                                                                                
                                                                          
    g_boot_millis = millis();
    g_boot_heap_initial = ESP.getFreeHeap();
    g_boot_block_initial = ESP.getMaxFreeBlockSize();
    g_boot_frag_initial = ESP.getHeapFragmentation();
    Serial.printf_P(PSTR("[BOOT] Heap=%u maxBlock=%u frag=%u%% BOOTID=%s BUILD=%s\n"),
                    g_boot_heap_initial, g_boot_block_initial, g_boot_frag_initial,
                    bootIdString().c_str(), SAR_BUILD_ID);

BWC_LOG_P(PSTR("\nStart\n"),0);
    BWC_LOG_P(PSTR("Millis: %d @ line: %d\n"), millis(), __LINE__);
                             
    gotIpEventHandler = WiFi.onStationModeGotIP(cb_gotIP);
    disconnectedEventHandler = WiFi.onStationModeDisconnected(cb_disconnected);

    LittleFS.begin();
    const bool cloudV2CredsLoaded = cloudV2CredentialsLoad();
    Serial.printf_P(PSTR("[CloudV2] credentials: %s\n"), cloudV2CredsLoaded ? "provisioned" : "not provisioned");
    sarCloudV2MigrationLoadState();
    if (!cloudV2CredsLoaded) {
        sarSerialProvisioningBootWindow(10000UL);
    }
    if (cloudV2CredentialsProvisioned()) {
        sarCloudV2MigrationClearState();
    }
    loadRestartMarkerBoot();
    loadPresenceDiagBoot();
    loadPresenceCrashMarkerBoot();

                                                                                  
    {
        String tmp;
        tmp.reserve(896);
        tmp += F("BUILD: ");
        tmp += SAR_BUILD_ID;
        tmp += F("\nBOOTID: ");
        tmp += bootIdString();
        tmp += F("\nResetReason: ");
        tmp += ESP.getResetReason();
        tmp += F("\n\nResetInfo:\n");
        tmp += ESP.getResetInfo();
        tmp += F("\n\nStructuredResetInfo:\n");
#if defined(ESP8266)
        appendStructuredResetInfo(tmp);
#else
        tmp += F("not available\n");
#endif
        tmp += F("\nLastRestartMarker:\n");
        if (g_last_restart_marker_boot.length()) tmp += g_last_restart_marker_boot; else tmp += F("(none)");
        tmp += F("\n\nBootHeap: ");
        tmp += String(g_boot_heap_initial);
        tmp += F("  maxBlock: ");
        tmp += String(g_boot_block_initial);
        tmp += F("  frag: ");
        tmp += String(g_boot_frag_initial);
        tmp += F("%\nChipID: ");
        tmp += String(ESP.getChipId(), HEX);
        tmp += F("\nSDK: ");
        tmp += ESP.getSdkVersion();
        g_boot_diag = tmp;
    }

    {
        HeapSelectIram ephemeral;
                                                                     
        bwc = new BWC;
        oneWire = new OneWire(231);
        tempSensors = new DallasTemperature(oneWire);
    }
    bwc->setup();
    bwc->loop();
    serviceBwcLoopDiag();
    periodicTimer.attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
                                                                              
    if (SAR_LOCAL_WEBSOCKET_ENABLED) updateWSTimer.attach(2.0, []{ sendWSFlag = true; });
    loadWebConfig();

                                                                             
                                                                            
    startMqtt();

                         
                                                                                             
if (mqttCloudMode) {
  enableMqtt = useMqtt;
} else {
  startComplete_ticker.attach(30, []{ if(useMqtt) enableMqtt = true; startComplete_ticker.detach(); });
}

    startWiFi();
    if(bwc->hasTempSensor)
    { 
        oneWire->begin(bwc->tempSensorPin);
        tempSensors->begin();
    }
    bwc->print("   ");                                                   
    bwc->print(WiFi.localIP().toString());
    bwc->print("   ");
    bwc->print(FW_VERSION);
    Serial.println(F("End of setup()"));
    BWC_LOG_P(PSTR("Millis: %d @ line: %d\n"), millis(), __LINE__);
    heap_water_mark = ESP.getFreeHeap();
    Serial.println(ESP.getFreeHeap());
}

void loop(){
    uint32_t freeheap = ESP.getFreeHeap();
    if(freeheap < heap_water_mark) heap_water_mark = freeheap;
    heapGuardTick(millis());
    sarHandleSerialProvisioning();


                                                                            
                                                                   
                                                                        
                                 
    bwc->loop();
    serviceBwcLoopDiag();
                                                                                
    bool newData = bwc->newData();

                                                     
    if (WiFi.status() == WL_CONNECTED)
    {
                                      
        if (server) server->handleClient();
        sarRunPendingOnlineUpdate();

                                                                      
                                              
        sarCloudV2MigrationTick();

                                
        ArduinoOTA.handle();

                                                                       
                                                                                            
                                                               
        static bool presenceBootArmed = false;
        static uint32_t presenceBootArmStartMs = 0;
        if (presenceBootArmStartMs == 0) presenceBootArmStartMs = millis();

        if (mqttCloudMode && !SAR_CLOUD_V2_ALWAYS_ON && useMqtt && !presenceBootArmed) {
            if (WiFi.status() == WL_CONNECTED) {
                presenceBootArmed = true;
                presence_next_poll_ms = millis();                 
                if (PRESENCE_DEBUG) Serial.println(F("[CLOUD] Boot-Rearm: forcing presence poll now"));
            } else if ((uint32_t)(millis() - presenceBootArmStartMs) > 30000UL) {
                                                                                                
                presenceBootArmed = true;
                presence_next_poll_ms = millis();
                if (PRESENCE_DEBUG) Serial.println(F("[CLOUD] Boot-Rearm fallback: forcing presence poll now"));
            }
        }

                                                                                             
        if (mqttCloudMode && !SAR_CLOUD_V2_ALWAYS_ON) {
            static uint32_t nextGateTickMs = 0;
            if ((int32_t)(millis() - nextGateTickMs) >= 0) {
                nextGateTickMs = millis() + 2000UL;      
                updatePresenceGate();
                                                                                          
                                                                
                const uint32_t now = millis();
                if (cloudPollingEnabled()) {
                    if (presence_last_attempt_ms == 0) {
                        presence_last_attempt_ms = now;
                    }
                    if ((uint32_t)(now - presence_last_attempt_ms) > PRESENCE_NO_ATTEMPT_REARM_MS) {
                        presence_next_poll_ms = now;                        
                        presence_force_poll_ms = now;
                    }
                                                                                                    
                                                                                                                 
                    if (presence_last_ok_ms != 0 && (uint32_t)(now - presence_last_ok_ms) > PRESENCE_NO_OK_STALL_MS) {
                        presence_stall_count++;
                        presence_allowed = false;
                        presence_active_until_ms = 0;
                        presence_grace_until_ms  = 0;
                        cloud_pair_bootstrap_until_ms = 0;
                        presence_cost_guard_active = true;
                        presence_cost_guard_until_ms = now + MQTT_BACKOFF_ON_STALL_MS;
                        appendPresenceDiag(String(F("mqtt cost-guard stall disconnect ageMs=")) + String((uint32_t)(now - presence_last_ok_ms)), true);
                        cloud_next_mqtt_try_ms = now + MQTT_BACKOFF_ON_STALL_MS;

                                                                                       
                        if (presence_last_stall_rearm_ms == 0 || (uint32_t)(now - presence_last_stall_rearm_ms) > 30000UL) {
                            presence_last_stall_rearm_ms = now;
#if defined(ESP8266)
                            presenceClient.stop();
                            presenceTlsReady = false;
                            presenceMflnProbed = false;
                            presenceMflnOk = false;
#endif
                            presence_last_dns_ok = false;
                            presence_last_connect_ok = false;
                            presence_last_resolved_ip = IPAddress((uint32_t)0);
                            presence_last_http_status_line = "";
                            presence_last_body_byte = -1;
                            presence_last_ssl_err = 0;
                            presence_last_ip_fallback_used = false;
                            presence_last_force_ip_mode = false;
                            presence_host_anchor_fail_streak = 0;
                            presence_ip_preferred_until_ms = 0;
                            appendPresenceDiag(F("cloud soft rearm: presence stall watchdog"), true);
                        }

                                                                                            
                        presence_next_poll_ms = now;
                        presence_force_poll_ms = now;
                        presence_last_attempt_ms = 0;
                    }
                }

            }
        }

                                                                    
        delay(0);



       
if (mqttCloudMode || enableMqtt)
{
                                                                 
                                                             
                                                                 
    if (mqttCloudMode)
    {
        if (SAR_CLOUD_V2_ALWAYS_ON) {
            cloudV2MqttTick(newData);
        } else {
        const uint32_t nowCloud = millis();
        const uint32_t presenceOkAgeMs = presenceLastOkAgeMs(nowCloud);
        const bool presenceFresh = presenceFreshForMqtt(nowCloud);
        const bool bootstrapActive = cloudBootstrapActive();
        bool mqttShouldRun = presenceMqttShouldRunNow(nowCloud);

        if (!bootstrapActive && presence_allowed && !presenceFresh && presenceOkAgeMs != 0xFFFFFFFFUL && presenceOkAgeMs > PRESENCE_MQTT_FRESH_MAX_AGE_MS)
        {
            presence_allowed = false;
            presence_last_allowed_value = false;
            presence_active_until_ms = 0;
            presence_grace_until_ms  = 0;
            presence_last_stale_guard_age_ms = presenceOkAgeMs;
            if (!presence_cost_guard_active)
            {
                presence_stale_guard_count++;
                appendPresenceDiag(String(F("mqtt stale guard -> force offline ageMs=")) + String(presenceOkAgeMs), true);
            }
            presence_cost_guard_active = true;
            presence_cost_guard_until_ms = nowCloud + 30000UL;
            if (mqttClient && mqttClient->connected()) publishStatusRetained("Asleep");
#if defined(ESP8266)
            cloudTlsStopBounded(true, "presence-stale-guard");
#else
            if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
            if (aWifiClient) aWifiClient->stop();
#endif
            cloud_next_mqtt_try_ms = nowCloud + 30000UL;
            mqttShouldRun = false;
        }

                                                                                                 
                                                                                                   
        if (!bootstrapActive && (!presence_allowed || !presenceFresh))
        {
            mqttShouldRun = false;
        }

        presence_last_mqtt_should_run = mqttShouldRun;

        if (mqttClient) mqttClient->loop();

        if (mqttShouldRun)
        {
            if (mqttClient && !mqttClient->connected())
            {
                const uint32_t nowTry = millis();
                if (cloud_next_mqtt_try_ms == 0 || (int32_t)(nowTry - cloud_next_mqtt_try_ms) >= 0)
                {
                    cloud_next_mqtt_try_ms = nowTry + 5000UL;
                    Serial.printf_P(PSTR("CLOUD: mqttConnect() (pres=%d boot=%d)\n"),
                                    presence_allowed ? 1 : 0,
                                    cloudBootstrapActive() ? 1 : 0);
                    mqttConnect();
                }
            }
        }
        else
        {
                                                                                     
            if (mqttClient && mqttClient->connected()) publishStatusRetained("Asleep");
#if defined(ESP8266)
            cloudTlsStopBounded(true, "presence-gate-offline");
#else
            if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
            if (aWifiClient) aWifiClient->stop();
#endif
        }

                                                                          
        if (mqttClient && mqttClient->connected())
        {
            uint32_t nowMs = millis();
            uint32_t intervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
            if (intervalMs > 0 && (int32_t)(nowMs - sar_cloud_next_telemetry_ms) >= 0)
            {
                sar_cloud_next_telemetry_ms = nowMs + intervalMs;
                sendMQTTFlag = true;
            }

                                                                      
            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);
            if (!msg.equals(prevButtonName))
            {
                if (mqttPublishChecked(String(mqttBaseTopic) + F("/button"), msg, false)) {
                    prevButtonName = msg;
                }
            }

            if (newData || sendMQTTFlag)
            {
                sendMQTT();
                sendMQTTFlag = false;
            }

            if (send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                mqttServiceTick();
                sendMQTTConfig();
                mqttServiceTick();
            }
        }
        }                                    
    }
                                                                 
                                                                  
                                                                 
    else
    {
                                                                     
static uint32_t custom_next_try_ms = 0;
static uint8_t  custom_fail_streak = 0;

                                                                           
if (custom_mqtt_kick && (uint32_t)(millis() - custom_mqtt_kick_at_ms) > 800UL) {
    custom_mqtt_kick = false;
    custom_next_try_ms = 0;                    
}

                                                              
if (enableMqtt && mqttClient && !mqttClient->connected()) {
                                                                              
    mqtt_telemetry_enabled = false;
    mqtt_next_telemetry_ms = 0;
    if ((int32_t)(millis() - custom_next_try_ms) >= 0) {

        mqttConnect();

        if (!g_mqtt_last_connect_ok) {
            if (custom_fail_streak < 10) custom_fail_streak++;

                                                   
            uint32_t backoff = 5000UL << (custom_fail_streak < 4 ? custom_fail_streak : 4);
            if (backoff > 60000UL) backoff = 60000UL;

            custom_next_try_ms = millis() + backoff;
        } else {
            custom_fail_streak = 0;
            custom_next_try_ms = millis() + 5000UL;                 
        }
    }
}

mqttServiceTick();
if (mqttClient && mqttClient->connected())
        {
            mqttServiceTick();
            customHaDiscoveryTick();
            mqttServiceTick();

            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);

                                                
            if (!msg.equals(prevButtonName))
            {
                const bool retainButton = true;
                if (mqttPublishChecked(String(mqttBaseTopic) + F("/button"), msg, retainButton)) {
                    prevButtonName = msg;
                }
            }

                                                                                                       
            if (!custom_ha_discovery_pending && (newData || sendMQTTFlag))
            {
                mqttServiceTick();
                sendMQTT();
                mqttServiceTick();
                sendMQTTFlag = false;
            }

            if (send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                mqttServiceTick();
                sendMQTTConfig();
                mqttServiceTick();
            }
        }
    }

#if defined(ESP8266)
    if (mqttCloudMode) {
        cloudMqttSupervisorTick();
    }
#endif
}
             
        if (SAR_LOCAL_WEBSOCKET_ENABLED && (newData || sendWSFlag))
        {
            sendWSFlag = false;
            sendWS();
        }
        else if (!SAR_LOCAL_WEBSOCKET_ENABLED)
        {
            sendWSFlag = false;
        }
    }

                          
    if (periodicTimerFlag)
    {
        periodicTimerFlag = false;
        if (WiFi.status() != WL_CONNECTED)
        {
            bwc->print(F("check network"));
                                                                   
        }
if (WiFi.status() == WL_CONNECTED)
{
                                                                         
    if (enableMqtt && !mqttCloudMode)
    {
        if (mqttClient && !mqttClient->loop())
        {
            mqttConnect();
        }
    }
}

                                                                                          
        setTemperatureFromSensor();

                   
                                      
                     
                           
            
                           
                                         
            
    }

    if(checkNTP_flag)
    {
        checkNTP_flag = false;
        checkNTP();
    }

                                                                                  
                                                                               
    if (bwc && bwc->reboot_time_t == 0 && timeLooksValid())
    {
        checkNTP();
    }

    if(CheckWiFi_flag)
    {
        CheckWiFi_flag = false;
        checkWiFi();
    }
                                                                            
                                                                                               
    static uint32_t btnSeqFirstMs = 0;
    static bool btnSeqTriggered = false;
    if (bwc->getBtnSeqMatch())
    {
        if (btnSeqFirstMs == 0) btnSeqFirstMs = millis();
        if (!btnSeqTriggered && (millis() - btnSeqFirstMs) > 2000)
        {
            btnSeqTriggered = true;
            Serial.println(F("[SYS] Button reset sequence confirmed -> resetting WiFi + restart"));
            resetWiFi();
            delay(1000);
            requestRestart(__FUNCTION__);
        }
    }
    else
    {
        btnSeqFirstMs = 0;
        btnSeqTriggered = false;
    }
                  
                                   
                                                                                 
}

                                          
void write_mem_stats_to_file()
{
    File file = LittleFS.open(F("memstats.txt"), "a");
    if (!file)
    {
        file.close();
        return;
    }
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    {
        HeapSelectIram ephemeral;
        file.printf_P(PSTR("Time: %s, IRam: free %d, frag %d, max block %d "),
            asctime(&timeinfo),
            ESP.getFreeHeap(), 
            ESP.getHeapFragmentation(),
            ESP.getMaxFreeBlockSize()
            );
    }
            
    file.printf_P(PSTR("DRam: free %d, frag %d, max block %d\n"),
        ESP.getFreeHeap(), 
        ESP.getHeapFragmentation(),
        ESP.getMaxFreeBlockSize()
        );
    file.close();
}
    


                
static const char* SAR_UPDATE_HOST     PROGMEM = SAR_PUBLIC_UPDATE_HOST;
static const char* SAR_UPDATE_INFO_URL PROGMEM = SAR_PUBLIC_UPDATE_INFO_URL;
static const char* SAR_DEFAULT_FW_URL  PROGMEM = "";
static const char* SAR_DEFAULT_FS_URL  PROGMEM = "";

static uint8_t  sarOtaPending = 0;                                          
static bool     sarOtaRunning = false;
static uint32_t sarOtaRequestedAt = 0;
static String   sarOtaUrl;
static String   sarOtaLastType = "";
static String   sarOtaLastStatus = "Bereit";
static String   sarOtaLastError = "";
static int      sarOtaProgress = 0;
static uint32_t sarOtaExpectedSize = 0;
static String   sarOtaExpectedSha256;

                                                                               
                                                          
#if defined(ESP8266)
static BearSSL::Session sarOtaTlsSession;
#endif

                                                                             
                                                                                            
static const uint32_t SAR_OTA_RTC_MAGIC = 0x5341524FUL;          
static const uint32_t SAR_OTA_RTC_SLOT  = 96;
static const char*    SAR_OTA_RESULT_FILE = "/sar_ota_result.json";

struct SarOtaRtcState {
    uint32_t magic;
    uint32_t flags;                                             
    uint32_t fwSize;
    uint32_t fsSize;
    uint32_t counter;
};

static const uint32_t SAR_OTA_HTTP_TIMEOUT_MS = 30000UL;
static const uint32_t SAR_OTA_DATA_TIMEOUT_MS = 20000UL;
static const uint8_t  SAR_OTA_MAX_RECONNECTS  = 4;

static int sarVersionPart(const String& version, uint8_t index)
{
    uint8_t current = 0;
    String part;
    for (uint16_t i = 0; i < version.length(); i++) {
        char c = version.charAt(i);
        if (c == '.') {
            if (current == index) break;
            current++;
            part = "";
        } else if (isDigit(c)) {
            part += c;
        } else if (part.length() > 0) {
            break;
        }
    }
    return (current == index) ? part.toInt() : 0;
}

static int sarCompareVersions(const String& a, const String& b)
{
    for (uint8_t i = 0; i < 3; i++) {
        int av = sarVersionPart(a, i);
        int bv = sarVersionPart(b, i);
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

struct SarUpdateInfo {
    bool ok = false;
    String version;
    String firmwareUrl;
    String littlefsUrl;
    uint32_t firmwareSize = 0;
    uint32_t littlefsSize = 0;
    String firmwareSha256;
    String littlefsSha256;
    String notes;
    String error;
};

static String sarJsonEscape(const String& in)
{
    String out;
    out.reserve(in.length() + 8);
    for (uint16_t i = 0; i < in.length(); i++) {
        char c = in.charAt(i);
        if (c == '"') out += F("\\\"");
        else if (c == '\\') out += F("\\\\");
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

static bool sarReadOtaRtcState(SarOtaRtcState& st)
{
    memset(&st, 0, sizeof(st));
    if (!ESP.rtcUserMemoryRead(SAR_OTA_RTC_SLOT, (uint32_t*)&st, sizeof(st))) return false;
    return st.magic == SAR_OTA_RTC_MAGIC;
}

static void sarWriteOtaRtcState(const SarOtaRtcState& st)
{
    ESP.rtcUserMemoryWrite(SAR_OTA_RTC_SLOT, (uint32_t*)&st, sizeof(st));
}

static void sarRememberOtaSuccess(uint8_t type, uint32_t size)
{
    SarOtaRtcState st;
    if (!sarReadOtaRtcState(st)) {
        memset(&st, 0, sizeof(st));
        st.magic = SAR_OTA_RTC_MAGIC;
    }
    if (type == 1) { st.flags |= 0x01; st.fwSize = size; }
    if (type == 2) { st.flags |= 0x02; st.fsSize = size; }
    st.counter++;
    sarWriteOtaRtcState(st);

                                                                                                 
    if (type == 1) {
        File f = LittleFS.open(SAR_OTA_RESULT_FILE, "w");
        if (f) {
            f.print(F("{\"fwOk\":")); f.print((st.flags & 0x01) ? F("true") : F("false"));
            f.print(F(",\"fsOk\":")); f.print((st.flags & 0x02) ? F("true") : F("false"));
            f.print(F(",\"fwSize\":")); f.print(st.fwSize);
            f.print(F(",\"fsSize\":")); f.print(st.fsSize);
            f.print(F(",\"counter\":")); f.print(st.counter);
            f.print(F("}"));
            f.close();
        }
    }
}

static void sarGetPersistedOtaResult(bool& fwOk, bool& fsOk, uint32_t& fwSize, uint32_t& fsSize)
{
    fwOk = false; fsOk = false; fwSize = 0; fsSize = 0;

                                                  
    File f = LittleFS.open(SAR_OTA_RESULT_FILE, "r");
    if (f) {
        DynamicJsonDocument doc(256);
        if (!deserializeJson(doc, f)) {
            fwOk = doc["fwOk"] | false;
            fsOk = doc["fsOk"] | false;
            fwSize = doc["fwSize"] | 0;
            fsSize = doc["fsSize"] | 0;
        }
        f.close();
    }

                                                                                 
    SarOtaRtcState st;
    if (sarReadOtaRtcState(st)) {
        if (st.flags & 0x01) { fwOk = true; if (st.fwSize) fwSize = st.fwSize; }
        if (st.flags & 0x02) { fsOk = true; if (st.fsSize) fsSize = st.fsSize; }

                                                                                                  
        File wf = LittleFS.open(SAR_OTA_RESULT_FILE, "w");
        if (wf) {
            wf.print(F("{\"fwOk\":")); wf.print(fwOk ? F("true") : F("false"));
            wf.print(F(",\"fsOk\":")); wf.print(fsOk ? F("true") : F("false"));
            wf.print(F(",\"fwSize\":")); wf.print(fwSize);
            wf.print(F(",\"fsSize\":")); wf.print(fsSize);
            wf.print(F(",\"counter\":")); wf.print(st.counter);
            wf.print(F("}"));
            wf.close();
        }
    }
}

static bool sarParseHttpsUrl(const String& url, String& host, String& path)
{
    const String prefix = F("https://");
    if (!url.startsWith(prefix)) return false;
    int start = prefix.length();
    int slash = url.indexOf('/', start);
    if (slash < 0) {
        host = url.substring(start);
        path = F("/");
    } else {
        host = url.substring(start, slash);
        path = url.substring(slash);
        if (path.length() == 0) path = F("/");
    }
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    return host.length() > 0;
}

static bool sarOtaUrlAllowed(const String& url)
{
    String host, path;
    if (!sarParseHttpsUrl(url, host, path)) return false;
    return host.equalsIgnoreCase(String(FPSTR(SAR_UPDATE_HOST)));
}

static bool sarOtaHashLooksValid(const String& value)
{
    if (value.length() != 64) return false;
    for (uint16_t i = 0; i < value.length(); i++) {
        const char c = value.charAt(i);
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

static String sarOtaNormalizeHash(String value)
{
    value.trim();
    value.toLowerCase();
    return value;
}

static bool sarOtaEnsureValidTime(uint32_t waitMs = 10000UL)
{
    if (timeLooksValid()) return true;
                                                                                  
                                                                                 
    startNTP();
    waitForValidTime(waitMs);
    return timeLooksValid();
}

static std::unique_ptr<BearSSL::WiFiClientSecure> sarMakeSecureClient(uint32_t timeoutMs, uint16_t rxSize = 4096, uint16_t txSize = 512)
{
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
                                   
                                                                     
                                                                             
    client->setTrustAnchors(&tlsCaStatic);
    client->setSession(&sarOtaTlsSession);
    client->setTimeout(timeoutMs);
    client->setBufferSizes(rxSize, txSize);
    if (timeLooksValid()) client->setX509Time(time(nullptr));
    return client;
}

static bool sarOtaWaitForWifi(uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - start) < timeoutMs) {
        delay(250);
        yield();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[SAR OTA] WLAN nicht verbunden"));
        return false;
    }
    return true;
}

static bool sarOtaResolveHost(const String& host, IPAddress& out, uint8_t retries = 3)
{
    for (uint8_t i = 1; i <= retries; i++) {
        if (WiFi.hostByName(host.c_str(), out)) {
            Serial.print(F("[SAR OTA] DNS OK: "));
            Serial.print(host);
            Serial.print(F(" -> "));
            Serial.println(out);
            return true;
        }
        Serial.print(F("[SAR OTA] DNS retry "));
        Serial.print(i);
        Serial.print(F("/"));
        Serial.print(retries);
        Serial.print(F(" fuer "));
        Serial.println(host);
        delay(500);
        yield();
    }
    Serial.print(F("[SAR OTA] DNS fehlgeschlagen fuer "));
    Serial.println(host);
    return false;
}

static String sarOtaFriendlyHttpError(int code, const String& detail)
{
    if (code == -4) {
        return F("Verbindung zum SmartAndRelax Update-Server momentan nicht moeglich – bitte erneut pruefen.");
    }
    if (code < 0) {
        return String(F("Netzwerk kurzzeitig nicht erreichbar. Details: ")) + detail;
    }
    return String(F("HTTP Fehler: ")) + code + F(" ") + detail;
}

static SarUpdateInfo sarFetchUpdateInfo()
{
    SarUpdateInfo info;
    info.firmwareUrl = FPSTR(SAR_DEFAULT_FW_URL);
    info.littlefsUrl = FPSTR(SAR_DEFAULT_FS_URL);

    if (!sarOtaWaitForWifi(8000)) {
        info.error = F("WLAN nicht verbunden");
        return info;
    }
    if (!sarOtaEnsureValidTime()) {
        info.error = F("Systemzeit noch nicht synchronisiert. Bitte 20 Sekunden warten und erneut pruefen.");
        return info;
    }

    String jsonUrl = String(FPSTR(SAR_UPDATE_INFO_URL));
    String host, path;
    if (!sarParseHttpsUrl(jsonUrl, host, path) || !host.equalsIgnoreCase(String(FPSTR(SAR_UPDATE_HOST)))) {
        info.error = F("Ungueltige Update-Server URL");
        return info;
    }

    IPAddress ip;
    sarOtaResolveHost(host, ip, 3);                                                        

    String lastError;
    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        Serial.print(F("[SAR OTA] Manifest attempt="));
        Serial.print(attempt);
        Serial.print(F(" heap="));
        Serial.print(ESP.getFreeHeap());
        Serial.print(F(" maxBlock="));
        Serial.println(ESP.getMaxFreeBlockSize());

        auto client = sarMakeSecureClient(20000, 2048, 512);
        HTTPClient http;
        http.setTimeout(20000);
        http.setReuse(false);
        http.useHTTP10(true);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(*client, jsonUrl)) {
            lastError = F("HTTP begin fuer Manifest fehlgeschlagen");
            http.end();
            delay(700);
            yield();
            continue;
        }

        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String payload = http.getString();
            http.end();

            DynamicJsonDocument doc(1536);
            DeserializationError err = deserializeJson(doc, payload);
            if (err) {
                info.error = String(F("Manifest JSON Fehler: ")) + err.c_str();
                return info;
            }

            info.version          = String(doc["version"] | "");
            info.firmwareUrl      = String(doc["firmware"] | info.firmwareUrl.c_str());
            info.littlefsUrl      = String(doc["littlefs"] | info.littlefsUrl.c_str());
            info.firmwareSize     = doc["firmware_size"] | 0UL;
            info.littlefsSize     = doc["littlefs_size"] | 0UL;
            info.firmwareSha256   = sarOtaNormalizeHash(String(doc["firmware_sha256"] | ""));
            info.littlefsSha256   = sarOtaNormalizeHash(String(doc["littlefs_sha256"] | ""));
            info.notes            = String(doc["notes"] | "");

            if (info.version.length() == 0) {
                info.error = F("Keine Versionsnummer im Update-Manifest gefunden");
                return info;
            }
            if (!sarOtaUrlAllowed(info.firmwareUrl) || !sarOtaUrlAllowed(info.littlefsUrl)) {
                info.error = F("Manifest enthaelt eine nicht erlaubte Update-URL");
                return info;
            }
            if (info.firmwareSize == 0 || info.littlefsSize == 0) {
                info.error = F("Dateigroesse fehlt im Update-Manifest");
                return info;
            }
            if (!sarOtaHashLooksValid(info.firmwareSha256) || !sarOtaHashLooksValid(info.littlefsSha256)) {
                info.error = F("SHA-256 Pruefsumme fehlt oder ist ungueltig");
                return info;
            }

            info.ok = true;
            return info;
        }

        lastError = sarOtaFriendlyHttpError(code, http.errorToString(code));
        Serial.print(F("[SAR OTA] Manifest fehlgeschlagen attempt="));
        Serial.print(attempt);
        Serial.print(F(" code="));
        Serial.print(code);
        Serial.print(F(" error="));
        Serial.println(lastError);
        http.end();

        delay(700 + attempt * 300);
        yield();
    }

    info.error = lastError.length() ? lastError : F("Versionspruefung fehlgeschlagen");
    return info;
}

static void sarSendUpdateJson(const SarUpdateInfo& info)
{
    bool updateAvailable = info.ok && (sarCompareVersions(String(FW_VERSION), info.version) < 0);
    String out = F("{\"ok\":");
    out += info.ok ? F("true") : F("false");
    out += F(",\"current\":\""); out += FW_VERSION;
    out += F("\",\"latest\":\""); out += sarJsonEscape(info.version);
    out += F("\",\"updateAvailable\":"); out += updateAvailable ? F("true") : F("false");
    out += F(",\"firmwareUrl\":\""); out += sarJsonEscape(info.firmwareUrl);
    out += F("\",\"littlefsUrl\":\""); out += sarJsonEscape(info.littlefsUrl);
    out += F("\",\"firmwareSize\":"); out += info.firmwareSize;
    out += F(",\"littlefsSize\":"); out += info.littlefsSize;
    out += F(",\"integrity\":"); out += info.ok ? F("true") : F("false");
    out += F(",\"source\":\"SmartAndRelax Update-Server\"");
    out += F(",\"notes\":\""); out += sarJsonEscape(info.notes);
    bool fwOk, fsOk; uint32_t fwSize, fsSize;
    sarGetPersistedOtaResult(fwOk, fsOk, fwSize, fsSize);
    out += F("\",\"firmwareInstalled\":"); out += fwOk ? F("true") : F("false");
    out += F(",\"littlefsInstalled\":"); out += fsOk ? F("true") : F("false");
    out += F(",\"firmwareInstalledSize\":"); out += fwSize;
    out += F(",\"littlefsInstalledSize\":"); out += fsSize;
    out += F(",\"error\":\""); out += sarJsonEscape(info.error);
    out += F("\"}");
    server->send(200, F("application/json"), out);
}

void handleOnlineUpdatePage()
{
    if (!handleFileRead(F("/updateonline.html"))) {
        server->send(404, F("text/plain"), F("updateonline.html fehlt im LittleFS"));
    }
}

void handleOnlineUpdateCheck()
{
    sarSendUpdateJson(sarFetchUpdateInfo());
}

void handleOnlineUpdateStatus()
{
    String out = F("{\"running\":"); out += sarOtaRunning ? F("true") : F("false");
    out += F(",\"pending\":"); out += sarOtaPending ? F("true") : F("false");
    out += F(",\"progress\":"); out += sarOtaProgress;
    out += F(",\"type\":\""); out += sarJsonEscape(sarOtaLastType);
    out += F("\",\"status\":\""); out += sarJsonEscape(sarOtaLastStatus);
    bool fwOk, fsOk; uint32_t fwSize, fsSize;
    sarGetPersistedOtaResult(fwOk, fsOk, fwSize, fsSize);
    out += F("\",\"firmwareInstalled\":"); out += fwOk ? F("true") : F("false");
    out += F(",\"littlefsInstalled\":"); out += fsOk ? F("true") : F("false");
    out += F(",\"firmwareInstalledSize\":"); out += fwSize;
    out += F(",\"littlefsInstalledSize\":"); out += fsSize;
    out += F(",\"error\":\""); out += sarJsonEscape(sarOtaLastError);
    out += F("\"}");
    server->send(200, F("application/json"), out);
}

static void sarQueueOnlineUpdate(uint8_t type, const String& url, uint32_t expectedSize, const String& expectedSha256, const String& label)
{
    sarOtaPending = type;
    sarOtaRequestedAt = millis();
    sarOtaUrl = url;
    sarOtaExpectedSize = expectedSize;
    sarOtaExpectedSha256 = sarOtaNormalizeHash(expectedSha256);
    sarOtaLastType = label;
    sarOtaLastError = "";
    sarOtaProgress = 0;
    sarOtaLastStatus = label + F(" Update wurde vorbereitet...");
}

static void sarSendStartUpdateResponse(bool ok, const String& msg, const String& err)
{
    String out = F("{\"ok\":");
    out += ok ? F("true") : F("false");
    out += F(",\"message\":\""); out += sarJsonEscape(msg);
    out += F("\",\"error\":\""); out += sarJsonEscape(err);
    out += F("\"}");
    server->send(ok ? 200 : 409, F("application/json"), out);
}

void handleOnlineUpdateFirmware()
{
    if (sarOtaRunning || sarOtaPending) {
        sarSendStartUpdateResponse(false, "", F("Es laeuft bereits ein Update."));
        return;
    }
    SarUpdateInfo info = sarFetchUpdateInfo();
    if (!info.ok) {
        sarSendStartUpdateResponse(false, "", info.error.length() ? info.error : F("Update-Manifest konnte nicht geladen werden."));
        return;
    }
    bool force = server->hasArg(F("force"));
    if (!force && sarCompareVersions(String(FW_VERSION), info.version) >= 0) {
        sarSendStartUpdateResponse(false, "", F("Keine neuere Firmware-Version verfuegbar. Fuer Test bitte Update erzwingen aktivieren."));
        return;
    }
    sarQueueOnlineUpdate(1, info.firmwareUrl, info.firmwareSize, info.firmwareSha256, F("Firmware"));
    sarSendStartUpdateResponse(true, F("Firmware-Update wurde gestartet. Bitte warten..."), "");
}

void handleOnlineUpdateLittleFS()
{
    if (sarOtaRunning || sarOtaPending) {
        sarSendStartUpdateResponse(false, "", F("Es laeuft bereits ein Update."));
        return;
    }
    SarUpdateInfo info = sarFetchUpdateInfo();
    if (!info.ok) {
        sarSendStartUpdateResponse(false, "", info.error.length() ? info.error : F("Update-Manifest konnte nicht geladen werden."));
        return;
    }
    sarQueueOnlineUpdate(2, info.littlefsUrl, info.littlefsSize, info.littlefsSha256, F("LittleFS"));
    sarSendStartUpdateResponse(true, F("LittleFS-Update wurde gestartet. Bitte warten..."), "");
}

static int sarReadHttpStatusCode(BearSSL::WiFiClientSecure& client)
{
    String status = client.readStringUntil('\n');
    status.trim();
    Serial.print(F("[SAR OTA] HTTP status: "));
    Serial.println(status);
    int firstSpace = status.indexOf(' ');
    if (firstSpace < 0) return -1;
    return status.substring(firstSpace + 1, firstSpace + 4).toInt();
}

static bool sarParseContentRangeStart(const String& contentRange, uint32_t& startOut)
{
                                                
    String s = contentRange;
    s.trim();
    if (!s.startsWith(F("bytes "))) return false;
    int dash = s.indexOf('-', 6);
    if (dash < 0) return false;
    String start = s.substring(6, dash);
    start.trim();
    if (start.length() == 0) return false;
    startOut = (uint32_t)strtoul(start.c_str(), nullptr, 10);
    return true;
}

static String sarSha256ToHex(const uint8_t digest[32])
{
                                                                         
    static const char SAR_HEX_DIGITS[] = "0123456789abcdef";
    char out[65];
    for (uint8_t i = 0; i < 32; i++) {
        out[i * 2]     = SAR_HEX_DIGITS[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = SAR_HEX_DIGITS[digest[i] & 0x0F];
    }
    out[64] = 0;
    return String(out);
}

static bool sarStreamHttpsUpdate(const String& url, int command, uint32_t expectedSize, const String& expectedSha256)
{
    sarOtaLastError = "";
    sarOtaProgress = 0;

    if (!sarOtaWaitForWifi(8000)) {
        sarOtaLastError = F("WLAN nicht verbunden");
        return false;
    }
    if (!sarOtaEnsureValidTime()) {
        sarOtaLastError = F("Systemzeit nicht synchronisiert – TLS-Pruefung nicht moeglich");
        return false;
    }
    if (!sarOtaUrlAllowed(url)) {
        sarOtaLastError = F("Update-URL ist nicht erlaubt");
        return false;
    }
    if (expectedSize == 0 || !sarOtaHashLooksValid(expectedSha256)) {
        sarOtaLastError = F("Update-Metadaten unvollstaendig (Groesse/SHA-256)");
        return false;
    }

    String host, path;
    if (!sarParseHttpsUrl(url, host, path)) {
        sarOtaLastError = F("Ungueltige HTTPS URL");
        return false;
    }

    IPAddress resolvedIp;
    if (!sarOtaResolveHost(host, resolvedIp, 3)) {
        sarOtaLastError = F("Update-Server konnte per DNS nicht aufgeloest werden");
        return false;
    }

    Serial.print(F("[SAR OTA] Stream precheck size=")); Serial.print(expectedSize);
    Serial.print(F(" command=")); Serial.print(command == U_FLASH ? F("U_FLASH") : F("U_FS"));
    Serial.print(F(" heap=")); Serial.print(ESP.getFreeHeap());
    Serial.print(F(" maxBlock=")); Serial.print(ESP.getMaxFreeBlockSize());
    Serial.print(F(" freeSketch=")); Serial.println(ESP.getFreeSketchSpace());

    if (command == U_FS) {
        Serial.println(F("[SAR OTA] LittleFS.end() before U_FS update"));
        LittleFS.end();
        delay(150);
        yield();
    }

    uint8_t buf[1024];
    uint32_t written = 0;
    uint32_t lastLogMs = millis();
    bool updateStarted = false;
    bool firstBytesChecked = false;
    uint8_t reconnects = 0;

    br_sha256_context shaCtx;
    br_sha256_init(&shaCtx);

    while (written < expectedSize) {
        const bool resume = written > 0;
        if (reconnects >= SAR_OTA_MAX_RECONNECTS) {
            sarOtaLastError = String(F("Zu viele Verbindungsabbrueche. Geschrieben=")) + written + F("/") + expectedSize;
            if (updateStarted) Update.end(false);
            if (command == U_FS) LittleFS.begin();
            return false;
        }
        reconnects++;

        auto client = sarMakeSecureClient(SAR_OTA_HTTP_TIMEOUT_MS, 4096, 512);
        client->setNoDelay(true);

        Serial.print(F("[SAR OTA] HTTPS stream connect #")); Serial.print(reconnects);
        Serial.print(F(" offset=")); Serial.print(written);
        Serial.print(F(" heap=")); Serial.print(ESP.getFreeHeap());
        Serial.print(F(" maxBlock=")); Serial.println(ESP.getMaxFreeBlockSize());

        if (!client->connect(host.c_str(), 443)) {
            char sslErr[96] = {0};
            int err = client->getLastSSLError();
            client->getLastSSLError(sslErr, sizeof(sslErr));
            sarOtaLastError = String(F("TLS-Verbindung zum Update-Server fehlgeschlagen: ")) + err + F(" ") + sslErr;
            Serial.print(F("[SAR OTA] ")); Serial.println(sarOtaLastError);
            client->stop();
            delay(800 + reconnects * 300);
            yield();
            continue;
        }

        client->setTimeout(SAR_OTA_HTTP_TIMEOUT_MS);
        client->print(F("GET ")); client->print(path); client->print(F(" HTTP/1.1\r\n"));
        client->print(F("Host: ")); client->print(host); client->print(F("\r\n"));
        client->print(F("User-Agent: SmartAndRelax-ESP8266-OTA/"));
        client->print(FW_VERSION);
        client->print(F("\r\n"));
        client->print(F("Accept: application/octet-stream\r\n"));
        client->print(F("Accept-Encoding: identity\r\n"));
        client->print(F("Cache-Control: no-cache\r\n"));
        if (resume) {
            client->print(F("Range: bytes=")); client->print(written); client->print(F("-\r\n"));
        }
        client->print(F("Connection: close\r\n\r\n"));

        int statusCode = sarReadHttpStatusCode(*client);
        if ((!resume && statusCode != 200) || (resume && statusCode != 206)) {
            sarOtaLastError = String(F("Unerwarteter HTTP-Status beim Update: ")) + statusCode +
                              (resume ? F(" (Range-Fortsetzung erwartet 206)") : F(" (200 erwartet)"));
            client->stop();
            Serial.print(F("[SAR OTA] ")); Serial.println(sarOtaLastError);
            delay(500);
            yield();
            continue;
        }

        int32_t contentLength = -1;
        bool chunked = false;
        String contentRange;
        String contentEncoding;
        uint16_t headerLines = 0;
        while (client->connected()) {
            String line = client->readStringUntil('\n');
            line.trim();
            if (line.length() == 0) break;
            headerLines++;
            String lower = line;
            lower.toLowerCase();
            if (lower.startsWith(F("content-length:"))) {
                String v = line.substring(line.indexOf(':') + 1); v.trim();
                contentLength = v.toInt();
            } else if (lower.startsWith(F("transfer-encoding:")) && lower.indexOf(F("chunked")) >= 0) {
                chunked = true;
            } else if (lower.startsWith(F("content-range:"))) {
                contentRange = line.substring(line.indexOf(':') + 1); contentRange.trim();
            } else if (lower.startsWith(F("content-encoding:"))) {
                contentEncoding = line.substring(line.indexOf(':') + 1); contentEncoding.trim(); contentEncoding.toLowerCase();
            }
            yield();
        }

        Serial.print(F("[SAR OTA] Headers lines=")); Serial.print(headerLines);
        Serial.print(F(" contentLength=")); Serial.print(contentLength);
        Serial.print(F(" range=")); Serial.print(contentRange);
        Serial.print(F(" encoding=")); Serial.println(contentEncoding);

        if (chunked) {
            sarOtaLastError = F("Update-Server liefert chunked Transfer-Encoding; feste Content-Length erforderlich");
            client->stop();
            if (updateStarted) Update.end(false);
            if (command == U_FS) LittleFS.begin();
            return false;
        }
        if (contentEncoding.length() && contentEncoding != F("identity")) {
            sarOtaLastError = F("Update-Datei darf nicht HTTP-komprimiert ausgeliefert werden");
            client->stop();
            if (updateStarted) Update.end(false);
            if (command == U_FS) LittleFS.begin();
            return false;
        }

        const uint32_t remaining = expectedSize - written;
        if (contentLength <= 0 || (uint32_t)contentLength > remaining) {
            sarOtaLastError = String(F("Ungueltige Content-Length: ")) + contentLength + F(" Rest=") + remaining;
            client->stop();
            if (updateStarted) Update.end(false);
            if (command == U_FS) LittleFS.begin();
            return false;
        }
        if (!resume && (uint32_t)contentLength != expectedSize) {
            sarOtaLastError = String(F("Dateigroesse stimmt nicht mit Manifest ueberein: HTTP=")) + contentLength + F(" Manifest=") + expectedSize;
            client->stop();
            if (command == U_FS) LittleFS.begin();
            return false;
        }
        if (resume) {
            uint32_t rangeStart = 0;
            if (!sarParseContentRangeStart(contentRange, rangeStart) || rangeStart != written) {
                sarOtaLastError = String(F("Content-Range passt nicht zur Fortsetzung. Erwartet=")) + written + F(" erhalten=") + contentRange;
                client->stop();
                if (updateStarted) Update.end(false);
                if (command == U_FS) LittleFS.begin();
                return false;
            }
        }

        uint32_t gotThisConnection = 0;
        uint32_t lastDataMs = millis();
        while (gotThisConnection < (uint32_t)contentLength && written < expectedSize) {
            int avail = client->available();
            if (avail <= 0) {
                if (!client->connected()) break;
                if ((uint32_t)(millis() - lastDataMs) > SAR_OTA_DATA_TIMEOUT_MS) break;
                delay(4);
                yield();
                continue;
            }

            size_t want = (size_t)min((uint32_t)sizeof(buf), min((uint32_t)avail, expectedSize - written));
            uint32_t connectionRemaining = (uint32_t)contentLength - gotThisConnection;
            if ((uint32_t)want > connectionRemaining) want = (size_t)connectionRemaining;
            int got = client->readBytes((char*)buf, want);
            if (got <= 0) {
                delay(4);
                yield();
                continue;
            }

            lastDataMs = millis();

            if (!firstBytesChecked) {
                firstBytesChecked = true;
                if (command == U_FLASH && buf[0] != 0xE9) {
                    sarOtaLastError = F("Firmware-Datei ist keine gueltige ESP8266 firmware.bin (Magic Byte != 0xE9)");
                    client->stop();
                    if (command == U_FS) LittleFS.begin();
                    return false;
                }
            }

            if (!updateStarted) {
                Serial.print(F("[SAR OTA] Update.begin size=")); Serial.print(expectedSize);
                Serial.print(F(" heap=")); Serial.print(ESP.getFreeHeap());
                Serial.print(F(" maxBlock=")); Serial.println(ESP.getMaxFreeBlockSize());
                if (!Update.begin(expectedSize, command)) {
                    sarOtaLastError = String(F("Update.begin fehlgeschlagen: ")) + Update.getErrorString() +
                                      F(" heap=") + ESP.getFreeHeap() +
                                      F(" maxBlock=") + ESP.getMaxFreeBlockSize() +
                                      F(" freeSketch=") + ESP.getFreeSketchSpace();
                    client->stop();
                    if (command == U_FS) LittleFS.begin();
                    return false;
                }
                Update.runAsync(true);
                updateStarted = true;
                sarOtaLastStatus = sarOtaLastType + F(" Download laeuft – SHA-256 wird geprueft...");
            }

            size_t w = Update.write(buf, (size_t)got);
            if (w != (size_t)got) {
                sarOtaLastError = String(F("Flash write fehlgeschlagen bei ")) + written + F("/") + expectedSize +
                                  F(" got=") + got + F(" wrote=") + w + F(" error=") + Update.getErrorString();
                Update.end(false);
                client->stop();
                if (command == U_FS) LittleFS.begin();
                return false;
            }

            br_sha256_update(&shaCtx, buf, (size_t)got);
            written += (uint32_t)w;
            gotThisConnection += (uint32_t)got;
            sarOtaProgress = (uint8_t)((written * 100UL) / expectedSize);

            if ((uint32_t)(millis() - lastLogMs) > 3000UL) {
                lastLogMs = millis();
                Serial.print(F("[SAR OTA] Progress ")); Serial.print(written); Serial.print('/'); Serial.print(expectedSize);
                Serial.print(F(" ")); Serial.print(sarOtaProgress); Serial.print(F("% heap=")); Serial.print(ESP.getFreeHeap());
                Serial.print(F(" maxBlock=")); Serial.println(ESP.getMaxFreeBlockSize());
            }
            yield();
        }

        client->stop();

        Serial.print(F("[SAR OTA] Connection done got=")); Serial.print(gotThisConnection);
        Serial.print(F(" total=")); Serial.print(written); Serial.print('/'); Serial.println(expectedSize);

        if (written >= expectedSize) break;

                                                                                      
        sarOtaLastStatus = sarOtaLastType + F(" Verbindung unterbrochen – Download wird fortgesetzt...");
        Serial.print(F("[SAR OTA] Stream interrupted, resume at byte ")); Serial.println(written);
        delay(700);
        yield();
    }

    if (!updateStarted || written != expectedSize) {
        sarOtaLastError = String(F("Download unvollstaendig. Geschrieben=")) + written + F("/") + expectedSize;
        if (updateStarted) Update.end(false);
        if (command == U_FS) LittleFS.begin();
        return false;
    }

    uint8_t digest[32];
    br_sha256_out(&shaCtx, digest);
    String actualSha = sarSha256ToHex(digest);
    String wantedSha = sarOtaNormalizeHash(expectedSha256);
    Serial.print(F("[SAR OTA] SHA256 actual=")); Serial.println(actualSha);

    if (!actualSha.equalsIgnoreCase(wantedSha)) {
        sarOtaLastError = F("SHA-256 Pruefung fehlgeschlagen – Update wird aus Sicherheitsgruenden verworfen");
        Update.end(false);
        if (command == U_FS) LittleFS.begin();
        return false;
    }

    sarOtaLastStatus = sarOtaLastType + F(" SHA-256 OK – Update wird abgeschlossen...");

    if (!Update.end()) {
        sarOtaLastError = String(F("Update.end fehlgeschlagen: ")) + Update.getErrorString();
        if (command == U_FS) LittleFS.begin();
        return false;
    }
    if (!Update.isFinished()) {
        sarOtaLastError = F("Update nicht vollstaendig abgeschlossen");
        if (command == U_FS) LittleFS.begin();
        return false;
    }

    sarRememberOtaSuccess(command == U_FLASH ? 1 : 2, expectedSize);
    sarOtaProgress = 100;
    sarOtaLastStatus = sarOtaLastType + F(" Update erfolgreich. Neustart...");
    return true;
}

static void sarPrepareForOnlineUpdate()
{
    Serial.print(F("[SAR OTA] Prepare: heap=")); Serial.print(ESP.getFreeHeap());
    Serial.print(F(" maxBlock=")); Serial.println(ESP.getMaxFreeBlockSize());

    if (webSocket) {
        webSocket->disconnect();
        webSocket->close();
    }
#if defined(ESP8266)
    if (mqttCloudMode) cloudTlsStopBounded(true, "online-ota-prepare");
    else {
        if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
        if (aWifiClient) aWifiClient->stop();
    }
#else
    if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
    if (aWifiClient) aWifiClient->stop();
#endif
    presenceClient.stop();

                                                                                  
                                                                                     
    if (server) server->close();

    for (uint8_t i = 0; i < 20; i++) {
        delay(50);
        yield();
    }

    Serial.print(F("[SAR OTA] Prepared: heap=")); Serial.print(ESP.getFreeHeap());
    Serial.print(F(" maxBlock=")); Serial.println(ESP.getMaxFreeBlockSize());
}

void sarRunPendingOnlineUpdate()
{
    if (!sarOtaPending || sarOtaRunning) return;
    if ((uint32_t)(millis() - sarOtaRequestedAt) < 2000UL) return;

    uint8_t job = sarOtaPending;
    sarOtaPending = 0;
    sarOtaRunning = true;
    sarOtaLastError = "";
    sarOtaProgress = 0;
    sarOtaLastStatus = String(sarOtaLastType) + F(" Update laeuft. Bitte nicht ausschalten...");

    Serial.println(F("[SAR OTA] Online update starting (SmartAndRelax Update-Server)"));
    Serial.print(F("[SAR OTA] Type: ")); Serial.println(sarOtaLastType);
    Serial.print(F("[SAR OTA] URL: ")); Serial.println(sarOtaUrl);
    Serial.print(F("[SAR OTA] Expected size: ")); Serial.println(sarOtaExpectedSize);

    sarPrepareForOnlineUpdate();

    bool ok = false;
    if (job == 1) ok = sarStreamHttpsUpdate(sarOtaUrl, U_FLASH, sarOtaExpectedSize, sarOtaExpectedSha256);
    else if (job == 2) ok = sarStreamHttpsUpdate(sarOtaUrl, U_FS, sarOtaExpectedSize, sarOtaExpectedSha256);

    if (ok) {
        Serial.println(F("[SAR OTA] Update OK, rebooting"));
        delay(1000);
        requestRestart(job == 1 ? "OTA_SUCCESS firmware" : "OTA_SUCCESS filesystem");
    } else {
        sarOtaLastStatus = String(sarOtaLastType) + F(" Update fehlgeschlagen");
        Serial.print(F("[SAR OTA] Update failed: ")); Serial.println(sarOtaLastError);
        if (job == 2) LittleFS.begin();
                                                                                       
                                                                           
        delay(2000);
        requestRestart(job == 1 ? "OTA_FAILED firmware recovery" : "OTA_FAILED filesystem recovery");
    }

    sarOtaRunning = false;
}


   
                                                                                                 
   
void sendWS()
{
    if (!webSocket || !bwc) return;
    if(webSocket->connectedClients() == 0) return;
                                               
                                                                            
                                                                        
                                                                         
                                                          
                  
    String json;
    json.reserve(384);

    bwc->getJSONStates(json);
    webSocket->broadcastTXT(json);
                 
    json.clear();
    bwc->getJSONTimes(json);
    webSocket->broadcastTXT(json);
                      
    json.clear();
    getOtherInfo(json);
    webSocket->broadcastTXT(json);
                                  
                                     
                                  
                          
                                 
                                      
                                        
}

void getOtherInfo(String &rtn)
{
                                    
    StaticJsonDocument<640> doc;
                                     
    doc[F("CONTENT")] = F("OTHER");
    doc[F("MQTT")] = (mqttClient ? mqttClient->state() : 999);
                        
                                                           
    doc[F("HASJETS")] = bwc->hasjets;
    doc[F("HASGOD")] = bwc->hasgod;
    doc[F("MODEL")] = bwc->getModel();
    doc[F("RSSI")] = WiFi.RSSI();
    doc[F("IP")] = WiFi.localIP().toString();
    doc[F("SSID")] = WiFi.SSID();
    doc[F("FW")] = FW_VERSION;
    doc[F("BUILD")] = SAR_BUILD_ID;
    doc[F("BOOTID")] = bootIdString();
                                                                         
                                                                          
    doc[F("REMOTE_PWR_LOCK")] = 1;
    doc[F("loopfq")] = bwc->loop_count;
    bwc->loop_count = 0;

                               
    if (serializeJson(doc, rtn) == 0)
    {
        rtn = F("{\"error\": \"Failed to serialize other\"}");
    }
}

/** @author 877dev */







void sendMQTT()
{
    if (!mqttClient || !mqttClient->connected()) return;
    if (mqtt_ha_discovery_active) return;                                                    

    String json;
    json.reserve(320);

                                                            
                                                                  
    const bool retainTelemetry = !mqttCloudMode;

                                
    bwc->getJSONStates(json);
    if (mqttPublishChecked(String(mqttBaseTopic) + F("/message"), json, retainTelemetry))
    {
        BWC_LOG_P(PSTR("MQTT > message published\n"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > message not published"),0);
        return;
    }

                             
                                                                
                                               
    if (!mqttCloudMode)
    {
        json.clear();
        bwc->getJSONTimes(json);
        if (mqttPublishChecked(String(mqttBaseTopic) + F("/times"), json, retainTelemetry))
        {
            BWC_LOG_P(PSTR("MQTT > times published"),0);
        }
        else
        {
            BWC_LOG_P(PSTR("MQTT > times not published"),0);
            return;
        }
    }

                             
                                                            
    json.clear();
    getOtherInfo(json);
    if (mqttPublishChecked(String(mqttBaseTopic) + F("/other"), json, retainTelemetry))
    {
        BWC_LOG_P(PSTR("MQTT > other published"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > other not published"),0);
    }
}


void sendMQTTConfig()
{
    if (mqtt_ha_discovery_active) return;                                           
    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);

    const bool retainCfg = !mqttCloudMode;                                       

    (void)mqttPublishChecked(String(mqttBaseTopic) + F("/get_config"), json, retainCfg);
}


   
                                                                              
                                               
   
void startWiFi()
{
    BWC_LOG_P(PSTR("startWiFi() @ millis: %d\n"), millis());
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.hostname(netHostname);
    loadWifi();

    if (!wifi_info.enableStaticIp4 && mqttCloudMode && PRESENCE_FORCE_PUBLIC_DNS)
    {
        WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0), PRESENCE_DNS_PRIMARY, PRESENCE_DNS_SECONDARY);
        appendPresenceDiag(String(F("wifi public dns forced dns1=")) + PRESENCE_DNS_PRIMARY.toString() + F(" dns2=") + PRESENCE_DNS_SECONDARY.toString(), true);
    }

    if (wifi_info.enableStaticIp4)
    {
        BWC_LOG_P(PSTR("Setting static IP\n"),0);
        IPAddress ip4Address;
        IPAddress ip4Gateway;
        IPAddress ip4Subnet;
        IPAddress ip4DnsPrimary;
        IPAddress ip4DnsSecondary;
        ip4Address.fromString(wifi_info.ip4Address_str);
        ip4Gateway.fromString(wifi_info.ip4Gateway_str);
        ip4Subnet.fromString(wifi_info.ip4Subnet_str);
        ip4DnsPrimary.fromString(wifi_info.ip4DnsPrimary_str);
        ip4DnsSecondary.fromString(wifi_info.ip4DnsSecondary_str);
        BWC_LOG_P(PSTR("WiFi > using static IP %s on gateway %s\n"),ip4Address.toString(), ip4Gateway.toString());
        WiFi.config(ip4Address, ip4Gateway, ip4Subnet, ip4DnsPrimary, ip4DnsSecondary);
    }

    if (wifi_info.enableAp)
    {
        BWC_LOG_P(PSTR("WiFi > using WiFi configuration with SSID %s\n"), wifi_info.apSsid);

        WiFi.begin(wifi_info.apSsid.c_str(), wifi_info.apPwd.c_str());
        checkWifi_ticker.attach(2.0, checkWiFi_ISR);
        Serial.println(F("WiFi > Trying to connect ..."));
    }
    else
    {
        startWiFiConfigPortal();
    }

}

void checkWiFi_ISR()
{
    CheckWiFi_flag = true;
}

void checkWiFi()
{
    const int maxTries = 30;
    static uint8_t tryCount = 0;

    if (WiFi.status() == WL_CONNECTED)
    {
        checkWifi_ticker.detach();
        wifi_info.enableAp = true;
        wifi_info.apSsid = WiFi.SSID();
        wifi_info.apPwd = WiFi.psk();
        saveWifi();
        return;
    }

    if (++tryCount >= maxTries)
    {
        if (wifi_info.enableWmApFallback)
        {
                                           
            wifi_info.enableAp = false;
            wifi_info.enableStaticIp4 = false;
                                             
            startWiFiConfigPortal();
        }
    }
}

   
                                         
   
void startWiFiConfigPortal()
{
    Serial.println(F("WiFi > Using WiFiManager Config Portal"));
    ESP_WiFiManager wm;
    wm.autoConnect(wmApName, wmApPassword);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }

                                                              
                                                                                               
                                                                                             
                                              
    wifi_info.enableAp = true;
    wifi_info.apSsid = WiFi.SSID();
    wifi_info.apPwd = WiFi.psk();
    saveWifi();
}

void checkNTP_ISR()
{
    checkNTP_flag = true;
}

void checkNTP()
{
    if (bwc && bwc->reboot_time_t != 0) {
        if (ntpCheck_ticker.active()) ntpCheck_ticker.detach();
        return;
    }

    time_t now = time(nullptr);
    static uint8_t ntpTryNumber = 0;
    if(now < 8 * 3600 * 2)
    {
        if (++ntpTryNumber == 10) {
            ntpTryNumber = 0;                         
            ntpCheck_ticker.detach();
        }
        return;
    }
    ntpCheck_ticker.detach();                                 
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    time_t boot_timestamp = getBootTime();
    tm * boot_time_tm = gmtime(&boot_timestamp);
    char boot_time_str[64];
    strftime(boot_time_str, 64, "%F %T", boot_time_tm);
    bwc->reboot_time_str = String(boot_time_str);
    bwc->reboot_time_t = boot_timestamp;
    bwc->saveRebootInfo();
                                                                            
                                                                                    
    if (mqttCloudMode) cloud_v2_publish_config_pending = true;
}

   
                 
   
void startNTP()
{
    Serial.println(F("start NTP"));
    const char* ntp1 = (wifi_info.ip4NTP_str.length() > 0) ? wifi_info.ip4NTP_str.c_str() : "pool.ntp.org";
    configTime(0, 0, ntp1, "time.google.com", "time.cloudflare.com");
    ntpCheck_ticker.attach(0.5, checkNTP_ISR);
}

void startOTA()
{
    ArduinoOTA.setHostname(OTAName);
    ArduinoOTA.setPassword(OTAPassword);

    ArduinoOTA.onStart([]() {
                                            
        stopall();
    });
    ArduinoOTA.onEnd([]() {
                                          
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
                                                                                 
    });
    ArduinoOTA.onError([](ota_error_t error) {
                                                     
                                                                         
                                                                                
                                                                                    
                                                                                    
                                                                            
    });
    ArduinoOTA.begin();
                                        
}

void stopall()
{
    Serial.printf_P(PSTR("Free mem before stop: %d\n"), ESP.getFreeHeap());
    bwc->stop();

    Serial.println(F("detaching"));
    periodicTimer.detach();
    updateWSTimer.detach();
    if (ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    if (checkWifi_ticker.active()) checkWifi_ticker.detach();

                      
                                        
                                    

                       
    Serial.println(F("stopping mqtt"));

                                                                             
#if defined(ESP8266)
    if (mqttCloudMode) cloudTlsStopBounded(true, "stopall");
    else if (mqttClient && mqttClient->connected()) mqttClient->disconnect();

    aWifiClient = tlsClient;                                   
                                                 
    tlsClient  = &tlsClientStatic;
    tlsCa      = &mqttCloudCaRootYeStatic;
    aWifiClient = tlsClient;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);
#else
    aWifiClient = &wifiClientStatic;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);
#endif


                                             
    presenceClient.stop();                        
                                       
    presenceTlsReady = false;



                                                                   
    Serial.println(F("stopping server"));
    if (server) { server->stop();                         
        server = nullptr; }

    Serial.println(F("stopping ws"));
    if (webSocket) { webSocket->close();                         
        webSocket = nullptr; }

    Serial.println(F("stopping FS"));
    LittleFS.end();

    Serial.println(F("end stopall"));
    Serial.printf_P(PSTR("Free mem after stop: %d\n"), ESP.getFreeHeap());
}


                                         
void pause_all(bool action)
{
    if(action)
    {
        if (!pause_all_diag_active) {
            pause_all_diag_active = true;
            pause_all_diag_started_ms = millis();
            pause_all_diag_count++;
        }
        if(periodicTimer.active()) periodicTimer.detach();
        if(startComplete_ticker.active()) startComplete_ticker.detach();
        if(updateWSTimer.active()) updateWSTimer.detach();
        if(bootlogTimer.active()) bootlogTimer.detach();
        if(ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    } else 
    {
        if (pause_all_diag_active) {
            pause_all_diag_active = false;
            pause_all_diag_last_duration_ms = (uint32_t)(millis() - pause_all_diag_started_ms);
            if (pause_all_diag_last_duration_ms > pause_all_diag_max_duration_ms) pause_all_diag_max_duration_ms = pause_all_diag_last_duration_ms;
        }
        periodicTimer.attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
        startComplete_ticker.attach(60, []{ if(useMqtt) enableMqtt = true; startComplete_ticker.detach(); });
        if (SAR_LOCAL_WEBSOCKET_ENABLED) updateWSTimer.attach(2.0, []{ sendWSFlag = true; });
                                                                                                                   
    }
    bwc->pause_all(action);
}

void startWebSocket()
{
                                                                        
                                                                           
                                                                             
    return;
}

   
                           
   
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t len)
{
                                           
    switch (type)
    {
                                           
        case WStype_DISCONNECTED:
                                                                    
        break;

                                                       
        case WStype_CONNECTED:
        {
                                                       
                                                                                                                                  
            sendWS();
        }
        break;

                                       
        case WStype_TEXT:
        {
                                                                                
                                            
            StaticJsonDocument<256> doc;
                                                                                            
            if (!payload || len == 0) return;
            DeserializationError error = deserializeJson(doc, payload, len);
            if (error)
            {
            Serial.println(F("WebSocket > JSON command failed"));
            return;
            }

                                                              
            Commands command = doc[F("CMD")];
            int64_t value = doc[F("VALUE")];
            int64_t xtime = doc[F("XTIME")];
            int64_t interval = doc[F("INTERVAL")];
            String txt = doc[F("TXT")] | "";
            command_que_item item;
            item.cmd = command;
            item.val = value;
            item.xtime = xtime;
            item.interval = interval;
            item.text = txt;
            if (bwc) bwc->add_command(item);
        }
        break;

        default:
        break;
    }
}

   
                                                          
   
void handleDiag();
void handleWsState();
void handleOnlineUpdatePage();
void handleOnlineUpdateCheck();
void handleOnlineUpdateFirmware();
void handleOnlineUpdateLittleFS();
void handleGetSmartSchedule();
void handleSetSmartSchedule();
void handleUpdateSmartSchedule();
void handleCancelSmartSchedule();

void startHttpServer()
{
    if(server != nullptr)
    {
#if defined(ESP8266)
        http_reuse_count++;
#endif
        return;
    }

    {
                                    
        server = new ESP8266WebServer(80);
        if (!server) {
            Serial.println(F("HTTP > allocation failed"));
            return;
        }
#if defined(ESP8266)
        http_init_count++;
#endif
        server->on(F("/diag"), handleDiag);
        server->on(F("/diag/"), handleDiag);
        server->on(F("/getconfig/"), handleGetConfig);
        server->on(F("/setconfig/"), handleSetConfig);
        server->on(F("/getcommands/"), handleGetCommandQueue);
        server->on(F("/addcommand/"), handleAddCommand);
        server->on(F("/editcommand/"), handleEditCommand);
        server->on(F("/delcommand/"), handleDelCommand);
        server->on(F("/getsmartschedule/"), HTTP_POST, handleGetSmartSchedule);
        server->on(F("/setsmartschedule/"), HTTP_POST, handleSetSmartSchedule);
        server->on(F("/updatesmartschedule/"), HTTP_POST, handleUpdateSmartSchedule);
        server->on(F("/cancelsmartschedule/"), HTTP_POST, handleCancelSmartSchedule);
        server->on(F("/getwebconfig/"), handleGetWebConfig);
        server->on(F("/setwebconfig/"), handleSetWebConfig);
        server->on(F("/wsstate/"), handleWsState);
        server->on(F("/updateonline"), HTTP_GET, handleOnlineUpdatePage);
        server->on(F("/updateonline/"), HTTP_GET, handleOnlineUpdatePage);
        server->on(F("/api/update/check"), HTTP_GET, handleOnlineUpdateCheck);
        server->on(F("/api/update/status"), HTTP_GET, handleOnlineUpdateStatus);
        server->on(F("/api/update/firmware"), HTTP_POST, handleOnlineUpdateFirmware);
        server->on(F("/api/update/littlefs"), HTTP_POST, handleOnlineUpdateLittleFS);
        server->on(F("/getwifi/"), handleGetWifi);
        server->on(F("/setwifi/"), handleSetWifi);
        server->on(F("/resetwifi/"), handleResetWifi);
        server->on(F("/getmqtt/"), handleGetMqtt);
        server->on(F("/setmqtt/"), handleSetMqtt);
        server->on(F("/dir/"), handleDir);
        server->on(F("/hwtest/"), handleHWtest);
        server->on(F("/inputs/"), handleInputs);
        server->on(F("/upload.html"), HTTP_POST, [](){
            server->send(200, F("text/plain"), "");
        }, handleFileUpload);
        server->on(F("/remove.html"), HTTP_POST, handleFileRemove);
        server->on(F("/remove/"), HTTP_GET, handleFileRemove);
        server->on(F("/restart/"), handleRestart);
        server->on(F("/metrics"), handlePrometheusMetrics);                      
        server->on(F("/info/"), handleESPInfo);
        server->on(F("/sethardware/"), handleSetHardware);
        server->on(F("/gethardware/"), handleGetHardware);
        server->on(F("/debug-on/"), [](){bwc->BWC_DEBUG = true; server->send(200, F("text/plain"), "ok");});
        server->on(F("/debug-off/"), [](){bwc->BWC_DEBUG = false; server->send(200, F("text/plain"), "ok");});
        server->on(F("/cmdq_file/"), handle_cmdq_file);

                                                                                      
                                       
        server->onNotFound(handleNotFound);
                                
        server->begin();
    }
    
                                                  
}

void handleGetHardware()
{
    if (!checkHttpPost(server->method())) return;
    File file = LittleFS.open(F("hwcfg.json"), "r");
    if (!file)
    {
                                                          
        server->send(404, F("text/plain"), F("not found"));
        return;
    }
    server->send(200, F("text/plain"), file.readString());
    file.close();
}

void handleSetHardware()
{
    if (!checkHttpPost(server->method())) return;
    String message = server->arg(0);
                                                              
    File file = LittleFS.open(F("hwcfg.json"), "w");
    if (!file)
    {
                                                          
        return;
    }
    file.print(message);
    file.close();
    server->send(200, F("text/plain"), "ok");
                                          
}

void preparefortest()
{
    for(int i = 0; i < 7; i++)
    {
        pinMode(bwc->pins[i], INPUT);
    }
}

void handleInputs()
{
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    bwc->stop();
    preparefortest();

    bool old_pin_state[7] = {0}, new_pin_state[7] = {0};
    int counter[7] = {0};
    unsigned long t = millis();                  

    while(millis() < t+5000)
    {
        for(uint8_t i = 0; i < 7; i++)
        {
            new_pin_state[i] = digitalRead(bwc->pins[i]);
            if(new_pin_state[i] != old_pin_state[i]) counter[i]++;
            old_pin_state[i] = new_pin_state[i];
        }
        yield();
    }

                                   
    char s[128];
    for(int i = 0; i < 7; i++)
    {
        sprintf_P(s, PSTR("Edges received on pin D%d: %d\n"), gpio2dp(bwc->pins[i]), counter[i]);
        server->sendContent(s);
    }
    sprintf_P(s, PSTR("On 6-w pump the highest number is CLK, next is DATA and third is CS. On 4-wires the highest is CIO or DSP TX to ESP."));
    server->sendContent(s);
    server->sendContent("");
    bwc->setup();
}

void handleHWtest()
{
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    int errors = 0;
    bool state = false;
    char result[128];

    bwc->stop();
    preparefortest();

    for(int i = 0; i < 10; i++)
    {
        sprintf_P(result, PSTR("\nConnect the cables now!\nStarting test in %d seconds...\n"), 10-i);
        server->sendContent(result);
        for(int t = 0; t < 512; t++)
            server->sendContent(" ");
        delay(1000);
    }

                                          
    sprintf_P(result, PSTR("Start test. Seq begins with HIGH, then alters.\n\n"));
    server->sendContent(result);
    for(int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin]), gpio2dp(bwc->pins[pin+3]));
        server->sendContent(result);
        pinMode(bwc->pins[pin], OUTPUT);
        pinMode(bwc->pins[pin+3], INPUT);
        for(int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin+3]) != state;
            errors += error;
            if(error)
                if(state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

                                   

    for(int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin+3]), gpio2dp(bwc->pins[pin]));
        server->sendContent(result);
        pinMode(bwc->pins[pin+3], OUTPUT);
        pinMode(bwc->pins[pin], INPUT);
        for(int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin+3], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin]) != state;
            errors += error;
            if(error)
                if(state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

    sprintf_P(result, PSTR("End of test!\n\"1\" or \"0\" indicates ERROR, depending on test state. \"-\" is good.\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Switching cio pins 5s HIGH -> 5s LOW -> input\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("then DSP pins 5s HIGH -> 5s LOW -> input, repeating\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Disconnect cables then reset chip when done!\n"));
    server->sendContent(result);

    server->sendContent("");
    while(true)
    {
                         
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], HIGH);
        }
        delay(5000);
                        
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], LOW);
        }
        delay(5000);
                         
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+0], INPUT);
            pinMode(bwc->pins[pin+3], OUTPUT);
            digitalWrite(bwc->pins[pin+3], HIGH);
        }
        delay(5000);
                        
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+0], INPUT);
            pinMode(bwc->pins[pin+3], OUTPUT);
            digitalWrite(bwc->pins[pin+3], LOW);
        }
        delay(5000);
    }
    bwc->setup();
}

static inline void serviceLocalHttpBackground()
{
                                                                           
                                                                         
                                                                             
    static uint32_t lastPumpServiceMs = 0;
    const uint32_t now = millis();
    if (bwc && (lastPumpServiceMs == 0 || (uint32_t)(now - lastPumpServiceMs) >= 10UL)) {
        lastPumpServiceMs = now;
        bwc->loop();
        serviceBwcLoopDiag();
    }
    delay(0);
}

void handleNotFound()
{
                                                                              
    if (!handleFileRead(server->uri()))
    {
        server->send(404, F("text/plain"), F("404: File Not Found"));
    }
}

String getContentType(const String& filename)
{
    if (filename.endsWith(".html")) return F("text/html");
    else if (filename.endsWith(".css")) return F("text/css");
    else if (filename.endsWith(".js")) return F("application/javascript");
    else if (filename.endsWith(".ico")) return F("image/x-icon");
    else if (filename.endsWith(".gz")) return F("application/x-gzip");
    else if (filename.endsWith(".json")) return F("application/json");
    return F("text/plain");
}

   
                                                   
   
bool handleFileRead(String path)
{
                                                                          
                                                                             
                                                                               
                                                                            

    const uint32_t entryHeap = ESP.getFreeHeap();
    const uint32_t entryBlock = ESP.getMaxFreeBlockSize();
    if (entryHeap < 6500UL || entryBlock < 4000UL) {
        server->sendHeader(F("Cache-Control"), F("no-store"));
        server->send(503, F("text/plain"), F("Web UI deferred: low heap, please retry"));
        return true;
    }

    if (path.endsWith("/")) path += F("index.html");

    if (path.equalsIgnoreCase("/mqtt.json") ||
        path.equalsIgnoreCase("/wifi.json") ||
        path.equalsIgnoreCase(SAR_MIGRATION_STATE_PATH))
    {
        server->send(403, F("text/plain"), F("Permission denied."));
        return false;
    }

    String contentType = getContentType(path);
    bool isGz = false;
    String pathWithGz = path + F(".gz");

    if (LittleFS.exists(pathWithGz)) isGz = true;
    else if (!LittleFS.exists(path)) return false;
    if (isGz) path = pathWithGz;

    File file = LittleFS.open(path, "r");
    if (!file) return false;

    http_file_serve_count++;
    strncpy(http_file_last_path, path.c_str(), sizeof(http_file_last_path) - 1);
    http_file_last_path[sizeof(http_file_last_path) - 1] = 0;

    const uint32_t started = millis();
    uint32_t lastProgress = started;
    const size_t fsize = file.size();

    if (isGz) server->sendHeader(F("Content-Encoding"), F("gzip"));
    server->sendHeader(F("Cache-Control"), F("no-cache"));
    server->setContentLength(fsize);
    server->send(200, contentType, "");

    WiFiClient client = server->client();
                                                                          
    client.setTimeout(500);
    static uint8_t buf[384];
    bool aborted = false;

    while (file.available() && client.connected())
    {
        int writable = client.availableForWrite();
        if (writable <= 0) {
            http_file_write_stall_count++;
            const uint32_t noProgress = (uint32_t)(millis() - lastProgress);
            if (noProgress > http_file_max_no_progress_ms) http_file_max_no_progress_ms = noProgress;
            if (noProgress > 5000UL) { aborted = true; break; }
            serviceLocalHttpBackground();
            delay(1);
            continue;
        }

        size_t want = (size_t)writable;
        if (want > sizeof(buf)) want = sizeof(buf);
        const size_t available = (size_t)file.available();
        if (want > available) want = available;
        if (want == 0) { serviceLocalHttpBackground(); continue; }

        const size_t n = file.read(buf, want);
        if (n == 0) { aborted = true; break; }

        size_t off = 0;
        while (off < n && client.connected())
        {
            int room = client.availableForWrite();
            if (room <= 0) {
                http_file_write_stall_count++;
                const uint32_t noProgress = (uint32_t)(millis() - lastProgress);
                if (noProgress > http_file_max_no_progress_ms) http_file_max_no_progress_ms = noProgress;
                if (noProgress > 5000UL) { aborted = true; break; }
                serviceLocalHttpBackground();
                delay(1);
                continue;
            }

            size_t chunk = n - off;
            if (chunk > (size_t)room) chunk = (size_t)room;
            const size_t w = client.write(buf + off, chunk);
            if (w > 0) {
                off += w;
                lastProgress = millis();
            } else {
                http_file_write_stall_count++;
            }
            serviceLocalHttpBackground();
        }
        if (aborted) break;
    }

    file.close();
    http_file_last_duration_ms = (uint32_t)(millis() - started);
    if (http_file_last_duration_ms > http_file_max_duration_ms) http_file_max_duration_ms = http_file_last_duration_ms;

    if (aborted) {
        http_file_abort_count++;
        client.stop(20);
    }
    return true;
}


   
                                 
   
bool checkHttpPost(HTTPMethod method)
{
    if (method != HTTP_POST)
    {
        server->send(405, F("text/plain"), F("Method not allowed."));
        return false;
    }
    return true;
}

   
                           
                                    
   

                                                                             
                                                                             
                                                                               
class DiagChunkWriter
{
public:
    explicit DiagChunkWriter(ESP8266WebServer* srv) : _srv(srv) { _buf.reserve(512); }

    DiagChunkWriter& operator+=(const String& v) { append(v.c_str(), v.length()); return *this; }
    DiagChunkWriter& operator+=(const char* v) { if (v) append(v, strlen(v)); return *this; }
    DiagChunkWriter& operator+=(const __FlashStringHelper* v)
    {
        if (!v) return *this;
        PGM_P ptr = reinterpret_cast<PGM_P>(v);
        while (true) {
            const char c = pgm_read_byte(ptr++);
            if (!c) break;
            appendChar(c);
        }
        return *this;
    }

    void flush()
    {
        if (!_srv || !_buf.length()) return;
        _srv->sendContent(_buf);
        _buf.remove(0);
        yield();
    }

private:
    static const size_t kFlushAt = 480;
    ESP8266WebServer* _srv;
    String _buf;

    void appendChar(char c)
    {
        _buf += c;
        if (_buf.length() >= kFlushAt) flush();
    }

    void append(const char* data, size_t len)
    {
        for (size_t i = 0; i < len; ++i) appendChar(data[i]);
    }
};

static void diagAppendHtmlEscaped(DiagChunkWriter& out, const String& in)
{
    for (size_t i = 0; i < in.length(); ++i) {
        switch (in[i]) {
            case '&': out += F("&amp;");  break;
            case '<': out += F("&lt;");   break;
            case '>': out += F("&gt;");   break;
            case '"': out += F("&quot;"); break;
            default: {
                char one[2] = { in[i], 0 };
                out += one;
                break;
            }
        }
    }
}

void handleDiag()
{
    if(server == nullptr) return;

                                                                     
    const uint32_t diagEntryHeap = ESP.getFreeHeap();
    const uint32_t diagEntryBlock = ESP.getMaxFreeBlockSize();
    const uint8_t diagEntryFrag = ESP.getHeapFragmentation();

    if (!server->chunkedResponseModeStart(200, F("text/html; charset=utf-8"))) {
        server->send(505, F("text/plain"), F("/diag requires HTTP/1.1"));
        return;
    }
    DiagChunkWriter out(server);

    out += F("<html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1'>"
             "<title>ESP8266 /diag</title></head><body>"
             "<h2>ESP8266 Diagnostics</h2><pre>");

    diagAppendHtmlEscaped(out, g_boot_diag);

    out += F("\n\n--- Live ---\nUptime(ms): ");
    out += String(millis());
    out += F("\nHeap: ");
    out += String(diagEntryHeap);
    out += F("\nmaxBlock: ");
    out += String(diagEntryBlock);
    out += F("\nfrag: ");
    out += String(diagEntryFrag);
    out += F("%\nrestartMarkerOneShot: 1\ndiagEntryHeap: ");
    out += String(diagEntryHeap);
    out += F("\ndiagEntryMaxBlock: ");
    out += String(diagEntryBlock);
    out += F("\ndiagEntryFrag: ");
    out += String(diagEntryFrag);
    out += F("%\ndiagStreamingHeapNow: ");
    out += String(ESP.getFreeHeap());
    out += F("\ndiagStreamingMaxBlockNow: ");
    out += String(ESP.getMaxFreeBlockSize());
    out += F("\nheapGuardLowActive: ");
    out += String(low_heap_since_ms != 0 ? 1 : 0);
    out += F("\nheapGuardLowAgeMs: ");
    out += String(low_heap_since_ms ? (uint32_t)(millis() - low_heap_since_ms) : 0UL);
    out += F("\nheapGuardRestartsThisBoot: ");
    out += String(heap_guard_restarts);
    out += F("\nminHeapSeen: ");
    out += String(min_heap_seen == 0xFFFFFFFFUL ? 0UL : min_heap_seen);
    out += F("\nminMaxBlockSeen: ");
    out += String(min_maxblock_seen == 0xFFFFFFFFUL ? 0UL : min_maxblock_seen);
    out += F("\nheapGuardTlsRuntimeMode: ");
    out += String(heap_guard_tls_runtime_mode ? 1 : 0);
    out += F("\nheapGuardCurrentHeapLimit: ");
    out += String(heap_guard_current_heap_limit);
    out += F("\nheapGuardCurrentBlockLimit: ");
    out += String(heap_guard_current_block_limit);
    out += F("\nWiFi: ");
    out += (WiFi.status() == WL_CONNECTED) ? F("connected") : F("not connected");
    out += F("\nIP: ");
    out += WiFi.localIP().toString();
    out += F("\nGateway: ");
    out += WiFi.gatewayIP().toString();
    out += F("\nDNS1: ");
    out += WiFi.dnsIP(0).toString();
    out += F("\nDNS2: ");
    out += WiFi.dnsIP(1).toString();
    out += F("\nRSSI: ");
    out += String(WiFi.RSSI());
    out += F("\nREBOOTTIME: ");
    out += String(bwc ? (uint32_t)bwc->reboot_time_t : 0UL);
    out += F("\nREBOOTTIME_STR: ");
    out += (bwc && bwc->reboot_time_str.length()) ? bwc->reboot_time_str : F("(pending NTP)");
    out += F("\nlastDnsOk: ");
    out += presence_last_dns_ok ? F("1") : F("0");
    out += F("\nlastResolvedIp: ");
    out += presence_last_resolved_ip.toString();
    out += F("\nlastConnectOk: ");
    out += presence_last_connect_ok ? F("1") : F("0");
    out += F("\nlastIpFallbackUsed: ");
    out += presence_last_ip_fallback_used ? F("1") : F("0");
    out += F("\nlastForceIpMode: ");
    out += presence_last_force_ip_mode ? F("1") : F("0");
    out += F("\nstickyIpRemainingMs: ");
    out += String(presenceStickyIpRemainingMs(millis()));
    out += F("\nhostAnchorFailStreak: ");
    out += String(presence_host_anchor_fail_streak);
    out += F("\npresenceAllowedCurrent: ");
    out += presence_allowed ? F("1") : F("0");
    out += F("\nlastAllowedValue: ");
    out += presence_last_allowed_value ? F("1") : F("0");
    out += F("\npresenceFreshForMqtt: ");
    out += presenceFreshForMqtt(millis()) ? F("1") : F("0");
    out += F("\npresenceLastOkAgeMs: ");
    out += String((presence_last_ok_ms == 0) ? 0UL : (uint32_t)(millis() - presence_last_ok_ms));
    out += F("\npresenceMqttFreshMaxAgeMs: ");
    out += String(PRESENCE_MQTT_FRESH_MAX_AGE_MS);
    out += F("\nmqttShouldRunCurrent: ");
    out += presenceMqttShouldRunNow(millis()) ? F("1") : F("0");
    out += F("\nuseMqttCurrent: ");
    out += useMqtt ? F("1") : F("0");
    out += F("\nenableMqttCurrent: ");
    out += enableMqtt ? F("1") : F("0");
    out += F("\ncloudV2AlwaysOn: ");
    out += SAR_CLOUD_V2_ALWAYS_ON ? F("1") : F("0");
    out += F("\ncloudV2Provisioned: ");
    out += cloudV2CredentialsProvisioned() ? F("1") : F("0");
    out += F("\nmqttClientConnected: ");
    out += (mqttClient && mqttClient->connected()) ? F("1") : F("0");
    out += F("\nmqttClientState: ");
    out += String(mqttClient ? mqttClient->state() : 999);
    out += F("\ncloudNextMqttTryInMs: ");
    {
        const uint32_t nowDiag = millis();
        const uint32_t waitDiag = (cloud_next_mqtt_try_ms != 0 && (int32_t)(cloud_next_mqtt_try_ms - nowDiag) > 0)
                                  ? (uint32_t)(cloud_next_mqtt_try_ms - nowDiag) : 0UL;
        out += String(waitDiag);
    }
#if defined(ESP8266)
    out += F("\n--- Cloud V2 connection diag ---");
    out += F("\nBUILD: ");
    out += SAR_BUILD_ID;
    out += F("\nBOOTID: ");
    out += bootIdString();
    out += F("\nwifiGotIpCount: ");
    out += String(wifi_got_ip_count);
    out += F("\nwifiDisconnectCount: ");
    out += String(wifi_disconnect_count);
    out += F("\ngotIpHeapBefore: ");
    out += String(got_ip_heap_before);
    out += F("\ngotIpHeapAfter: ");
    out += String(got_ip_heap_after);
    out += F("\ngotIpHeapDelta: ");
    out += String(got_ip_heap_delta);
    out += F("\ngotIpBlockBefore: ");
    out += String(got_ip_block_before);
    out += F("\ngotIpBlockAfter: ");
    out += String(got_ip_block_after);
    out += F("\ngotIpBlockDelta: ");
    out += String(got_ip_block_delta);
    out += F("\nhttpInitCount: ");
    out += String(http_init_count);
    out += F("\nhttpReuseCount: ");
    out += String(http_reuse_count);
    out += F("\nwsInitCount: ");
    out += String(ws_init_count);
    out += F("\nwsReuseCount: ");
    out += String(ws_reuse_count);
    out += F("\nlocalWebSocketEnabled: ");
    out += (SAR_LOCAL_WEBSOCKET_ENABLED ? F("1") : F("0"));
    out += F("\nlocalUiTransport: HTTP_POLLING");
    out += F("\nhttpFileServeCount: ");
    out += String(http_file_serve_count);
    out += F("\nhttpFileAbortCount: ");
    out += String(http_file_abort_count);
    out += F("\nhttpFileWriteStallCount: ");
    out += String(http_file_write_stall_count);
    out += F("\nhttpFileLastDurationMs: ");
    out += String(http_file_last_duration_ms);
    out += F("\nhttpFileMaxDurationMs: ");
    out += String(http_file_max_duration_ms);
    out += F("\nhttpFileMaxNoProgressMs: ");
    out += String(http_file_max_no_progress_ms);
    out += F("\nhttpFileLastPath: ");
    diagAppendHtmlEscaped(out, String(http_file_last_path));
    out += F("\nwsStateRequestCount: ");
    out += String(wsstate_request_count);
    out += F("\nwsStateLowHeapDeferCount: ");
    out += String(wsstate_low_heap_defer_count);
    out += F("\nmqttInitCount: ");
    out += String(mqtt_init_count);
    out += F("\nmqttReconfigureCount: ");
    out += String(mqtt_reconfigure_count);
    out += F("\ntlsAdmissionBlockedCount: ");
    out += String(tls_admission_blocked_count);
    out += F("\ntlsAdmissionLastHeap: ");
    out += String(tls_admission_last_heap);
    out += F("\ntlsAdmissionLastBlock: ");
    out += String(tls_admission_last_block);
    out += F("\ntlsAdmissionBlockedAgeMs: ");
    out += String(tls_admission_blocked_since_ms ? (uint32_t)(millis() - tls_admission_blocked_since_ms) : 0UL);
    out += F("\ntlsAdmissionLastBlockedAgeMs: ");
    out += String(tls_admission_last_blocked_ms ? (uint32_t)(millis() - tls_admission_last_blocked_ms) : 0UL);
    out += F("\ntlsAdmissionRecoveryMs: ");
    out += String(SAR_TLS_ADMISSION_RECOVERY_MS);
    out += F("\ntlsAdmissionRecoveryRestartsThisBoot: ");
    out += String(tls_admission_recovery_restarts);
    out += F("\nmqttTlsConnectTimeoutMs: ");
    out += String(SAR_MQTT_TLS_CONNECT_TIMEOUT_MS);
    out += F("\nbearSslTimeoutPatchActive: 1");
    out += F("\nbearSslPumpServiceHookPatchActive: 1");
    out += F("\ntlsPumpServiceCount: ");
    out += String(sar_tls_pump_service_count);
    out += F("\ntlsPumpServiceMaxGapMs: ");
    out += String(sar_tls_pump_service_max_gap_ms);
    out += F("\nmqttTlsRuntimeTimeoutMs: ");
    out += String(SAR_MQTT_TLS_RUNTIME_TIMEOUT_MS);
    out += F("\nmqttConnAckTimeoutSec: ");
    out += String(SAR_MQTT_CONNACK_TIMEOUT_S);
    out += F("\nmqttTlsTimeoutReapplyCount: ");
    out += String(mqtt_tls_timeout_reapply_count);
    out += F("\nmqttTlsStopCount: ");
    out += String(mqtt_tls_stop_count);
    out += F("\nmqttTlsLastStopDurationMs: ");
    out += String(mqtt_tls_last_stop_duration_ms);
    out += F("\nmqttTlsMaxStopDurationMs: ");
    out += String(mqtt_tls_max_stop_duration_ms);
    out += F("\nmqttSlowConnectCount: ");
    out += String(mqtt_slow_connect_count);
    out += F("\nmqttOver5sConnectCount: ");
    out += String(mqtt_over_5s_connect_count);
    out += F("\nmqttTlsStageGapMs: ");
    out += String(SAR_MQTT_STAGE_GAP_MS);
    out += F("\nmqttConnectAttemptInProgress: ");
    out += String(cloud_mqtt_attempt_in_progress ? 1 : 0);
    out += F("\nmqttStageNotBeforeInMs: ");
    out += String((cloud_mqtt_stage_not_before_ms &&
                   (int32_t)(cloud_mqtt_stage_not_before_ms - millis()) > 0)
                      ? (uint32_t)(cloud_mqtt_stage_not_before_ms - millis()) : 0UL);
    out += F("\ncloudDnsStageTimeoutMs: ");
    out += String(SAR_CLOUD_DNS_STAGE_TIMEOUT_MS);
    out += F("\ncloudDnsPreflightAttempts: ");
    out += String(cloud_dns_preflight_attempt_count);
    out += F("\ncloudDnsPreflightSuccesses: ");
    out += String(cloud_dns_preflight_success_count);
    out += F("\ncloudDnsPreflightFailures: ");
    out += String(cloud_dns_preflight_fail_count);
    out += F("\ncloudDnsPreflightLastDurationMs: ");
    out += String(cloud_dns_preflight_last_duration_ms);
    out += F("\ncloudDnsPreflightMaxDurationMs: ");
    out += String(cloud_dns_preflight_max_duration_ms);
    out += F("\ncloudDnsPreflightIp: ");
    out += cloud_dns_preflight_ip.toString();
    out += F("\ncloudDnsPreflightFreshForMs: ");
    out += String((cloud_dns_preflight_valid_until_ms && (int32_t)(cloud_dns_preflight_valid_until_ms - millis()) > 0)
                    ? (uint32_t)(cloud_dns_preflight_valid_until_ms - millis()) : 0UL);
    out += F("\ncloudTcpStageTimeoutMs: ");
    out += String(SAR_CLOUD_TCP_STAGE_TIMEOUT_MS);
    out += F("\nmqttTcpPreconnectAttempts: ");
    out += String(mqtt_tcp_preconnect_attempt_count);
    out += F("\nmqttTcpPreconnectSuccesses: ");
    out += String(mqtt_tcp_preconnect_success_count);
    out += F("\nmqttTcpPreconnectFailures: ");
    out += String(mqtt_tcp_preconnect_fail_count);
    out += F("\nmqttTcpPreconnectLastDurationMs: ");
    out += String(mqtt_tcp_preconnect_last_duration_ms);
    out += F("\nmqttTcpPreconnectMaxDurationMs: ");
    out += String(mqtt_tcp_preconnect_max_duration_ms);
    out += F("\nmqttTcpPreconnectHeapBefore: ");
    out += String(mqtt_tcp_preconnect_heap_before);
    out += F("\nmqttTcpPreconnectHeapAfter: ");
    out += String(mqtt_tcp_preconnect_heap_after);
    out += F("\nmqttTcpPreconnectBlockBefore: ");
    out += String(mqtt_tcp_preconnect_block_before);
    out += F("\nmqttTcpPreconnectBlockAfter: ");
    out += String(mqtt_tcp_preconnect_block_after);
    out += F("\ncloudTcpStageReady: ");
    out += cloud_tcp_stage_ready ? F("1") : F("0");
    out += F("\nmqttTlsPreconnectAttempts: ");
    out += String(mqtt_tls_preconnect_attempt_count);
    out += F("\nmqttTlsPreconnectSuccesses: ");
    out += String(mqtt_tls_preconnect_success_count);
    out += F("\nmqttTlsPreconnectFailures: ");
    out += String(mqtt_tls_preconnect_fail_count);
    out += F("\nmqttTlsPreconnectLastDurationMs: ");
    out += String(mqtt_tls_preconnect_last_duration_ms);
    out += F("\nmqttTlsPreconnectMaxDurationMs: ");
    out += String(mqtt_tls_preconnect_max_duration_ms);
    out += F("\nmqttTlsPreconnectHeapBefore: ");
    out += String(mqtt_tls_preconnect_heap_before);
    out += F("\nmqttTlsPreconnectHeapAfter: ");
    out += String(mqtt_tls_preconnect_heap_after);
    out += F("\nmqttTlsPreconnectBlockBefore: ");
    out += String(mqtt_tls_preconnect_block_before);
    out += F("\nmqttTlsPreconnectBlockAfter: ");
    out += String(mqtt_tls_preconnect_block_after);
    out += F("\nmqttTlsPreconnectSlowCount: ");
    out += String(mqtt_tls_preconnect_slow_count);
    out += F("\nmqttTlsPreconnectOver4sCount: ");
    out += String(mqtt_tls_preconnect_over4s_count);
    out += F("\nmqttHandshakeLastDurationMs: ");
    out += String(mqtt_last_connect_duration_ms);
    out += F("\ntlsReclaimCount: ");
    out += String(tls_reclaim_count);
    out += F("\ntlsReclaimHeapBefore: ");
    out += String(tls_reclaim_heap_before);
    out += F("\ntlsReclaimHeapAfter: ");
    out += String(tls_reclaim_heap_after);
    out += F("\ntlsReclaimBlockBefore: ");
    out += String(tls_reclaim_block_before);
    out += F("\ntlsReclaimBlockAfter: ");
    out += String(tls_reclaim_block_after);
    out += F("\nmqttTlsSessionReuse: 1");
    out += F("\ntlsTimeDeferredCount: ");
    out += String(tls_time_deferred_count);
    out += F("\nmqttLastConnectDurationMs: ");
    out += String(mqtt_last_connect_duration_ms);
    out += F("\nmqttLastConnectHeapBefore: ");
    out += String(mqtt_last_connect_heap_before);
    out += F("\nmqttLastConnectHeapAfter: ");
    out += String(mqtt_last_connect_heap_after);
    out += F("\nmqttLastConnectBlockBefore: ");
    out += String(mqtt_last_connect_block_before);
    out += F("\nmqttLastConnectBlockAfter: ");
    out += String(mqtt_last_connect_block_after);
    out += F("\nwifiLastGotIpAgeMs: ");
    out += String(wifi_last_got_ip_ms ? (uint32_t)(millis() - wifi_last_got_ip_ms) : 0UL);
    out += F("\nwifiLastDisconnectAgeMs: ");
    out += String(wifi_last_disconnect_ms ? (uint32_t)(millis() - wifi_last_disconnect_ms) : 0UL);
    out += F("\nwifiLastDisconnectReason: ");
    out += String(wifi_last_disconnect_reason);
    out += F("\nmqttConnectAttempts: ");
    out += String(mqtt_connect_attempt_count);
    out += F("\nmqttConnectSuccesses: ");
    out += String(mqtt_connect_success_count);
    out += F("\nmqttConnectFailures: ");
    out += String(mqtt_connect_fail_count);
    out += F("\nmqttLastConnectTrigger: ");
    out += mqtt_last_connect_trigger;
    out += F("\nmqttCurrentSessionAgeMs: ");
    out += String((mqttClient && mqttClient->connected() && mqtt_last_connect_success_ms_diag)
                  ? (uint32_t)(millis() - mqtt_last_connect_success_ms_diag) : 0UL);
    out += F("\nmqttLoopDropCount: ");
    out += String(mqtt_loop_drop_count);
    out += F("\nmqttLastLoopDropAgeMs: ");
    out += String(mqtt_last_loop_drop_ms ? (uint32_t)(millis() - mqtt_last_loop_drop_ms) : 0UL);
    out += F("\nmqttLastLoopDropState: ");
    out += String(mqtt_last_loop_drop_state);
    out += F("\ntcpClientConnected: ");
    out += (aWifiClient && aWifiClient->connected()) ? F("1") : F("0");
#endif
    out += F("\nisPairedCurrent: ");
    out += isPaired() ? F("1") : F("0");
    out += F("\npairingCodeLen: ");
    out += String(mqttPairingCode.length());
    out += F("\ncostGuardActive: ");
    out += (presence_cost_guard_active && (presenceCostGuardRemainingMs(millis()) > 0)) ? F("1") : F("0");
    out += F("\ncostGuardRemainingMs: ");
    out += String(presenceCostGuardRemainingMs(millis()));
    out += F("\nlastMqttShouldRun: ");
    out += presence_last_mqtt_should_run ? F("1") : F("0");
    out += F("\nlastStaleGuardAgeMs: ");
    out += String(presence_last_stale_guard_age_ms);
    out += F("\nstaleGuardCount: ");
    out += String(presence_stale_guard_count);
    out += F("\nlastBodyByte: ");
    out += String(presence_last_body_byte);
    out += F("\nlastHttpStatusLine: ");
    diagAppendHtmlEscaped(out, presence_last_http_status_line);
    out += F("\npresenceHeapSkipFreeMin: ");
    out += String(PRESENCE_HEAP_SKIP_FREE_MS);
    out += F("\npresenceHeapSkipBlockMin: ");
    out += String(presence_heap_skip_block_min_current);
    out += F("\nlowHeapSkipCount: ");
    out += String(presence_low_heap_skip_count);
    out += F("\nlastSkipHeapFree: ");
    out += String(presence_last_skip_heap_free);
    out += F("\nlastSkipHeapBlock: ");
    out += String(presence_last_skip_heap_block);
    out += F("\npresenceAdaptiveBlockMinCurrent: ");
    out += String(presence_heap_skip_block_min_current);
    out += F("\npresenceAdaptiveBlockMinStart: ");
    out += String(PRESENCE_HEAP_SKIP_BLOCK_START_MS);
    out += F("\npresenceAdaptiveBlockMinFloor: ");
    out += String(PRESENCE_HEAP_SKIP_BLOCK_FLOOR_MS);
    out += F("\npresenceAdaptiveNearSkipCount: ");
    out += String(presence_adaptive_near_skip_count);
    out += F("\npresenceAdaptiveSuccessCount: ");
    out += String(presence_adaptive_success_count);
    out += F("\npresenceAdaptiveLowerCount: ");
    out += String(presence_adaptive_lower_count);
    out += F("\npresenceAdaptiveRaiseCount: ");
    out += String(presence_adaptive_raise_count);
    out += F("\npresenceAdaptiveLastStartHeap: ");
    out += String(presence_adaptive_last_start_heap);
    out += F("\npresenceAdaptiveLastStartBlock: ");
    out += String(presence_adaptive_last_start_block);
    out += F("\nlastSslErr: ");
    out += String(presence_last_ssl_err);

    out += F("\n\n--- Presence timing/crash diag ---");
    out += F("\npresencePollSeq: ");
    out += String(presence_poll_seq);
    out += F("\npresenceLastStage: ");
    diagAppendHtmlEscaped(out, String(presence_last_stage));
    out += F("\npresenceLastOutcome: ");
    out += String(presence_last_outcome);
    out += F("\npresenceLastPollDurationMs: ");
    out += String(presence_last_poll_duration_ms);
    out += F("\npresenceLastDnsDurationMs: ");
    out += String(presence_last_dns_duration_ms);
    out += F("\npresenceLastDnsTries: ");
    out += String(presence_last_dns_tries);
    out += F("\npresenceLastNtpWaitMs: ");
    out += String(presence_last_ntp_wait_ms);
    out += F("\npresenceLastTlsConnectMs: ");
    out += String(presence_last_tls_connect_ms);
    out += F("\npresenceLastTlsMode: ");
    out += String(presence_last_tls_mode);
    out += F("\npresenceLastHttpSendMs: ");
    out += String(presence_last_http_send_ms);
    out += F("\npresenceLastWaitFirstByteMs: ");
    out += String(presence_last_wait_first_byte_ms);
    out += F("\npresenceLastStatusReadMs: ");
    out += String(presence_last_status_read_ms);
    out += F("\npresenceLastHeaderReadMs: ");
    out += String(presence_last_header_read_ms);
    out += F("\npresenceLastBodyReadMs: ");
    out += String(presence_last_body_read_ms);
    out += F("\npresenceLastHttpDurationMs: ");
    out += String(presence_last_http_duration_ms);
    out += F("\npresenceLastContentLength: ");
    out += String(presence_last_content_len);
    out += F("\npresenceLastBytesRead: ");
    out += String(presence_last_bytes_read);
    out += F("\npresenceHeapStart: ");
    out += String(presence_last_heap_start);
    out += F("\npresenceHeapMinDuringPoll: ");
    out += String(presence_last_heap_min);
    out += F("\npresenceHeapEnd: ");
    out += String(presence_last_heap_end);
    out += F("\npresenceBlockStart: ");
    out += String(presence_last_block_start);
    out += F("\npresenceBlockMinDuringPoll: ");
    out += String(presence_last_block_min);
    out += F("\npresenceBlockEnd: ");
    out += String(presence_last_block_end);
    out += F("\npresenceFragStart: ");
    out += String(presence_last_frag_start);
    out += F("\npresenceFragEnd: ");
    out += String(presence_last_frag_end);
    out += F("\n\n--- Presence crash marker from previous boot ---\n");
    if (presence_crash_marker_boot.length()) diagAppendHtmlEscaped(out, presence_crash_marker_boot); else out += F("(none)");

    out += F("\n\n--- Pump communication diag ---");
    out += F("\nbwcLastLoopAgeMs: ");
    out += String((bwc_diag_last_loop_ms == 0) ? 0UL : (uint32_t)(millis() - bwc_diag_last_loop_ms));
    out += F("\nbwcLastLoopGapMs: ");
    out += String(bwc_diag_last_gap_ms);
    out += F("\nbwcMaxLoopGapMs: ");
    out += String(bwc_diag_max_gap_ms);
    out += F("\nbwcGapOver250Count: ");
    out += String(bwc_diag_gap_over_250);
    out += F("\nbwcGapOver500Count: ");
    out += String(bwc_diag_gap_over_500);
    out += F("\nbwcGapOver1000Count: ");
    out += String(bwc_diag_gap_over_1000);
    out += F("\nbwcGapOver3000Count: ");
    out += String(bwc_diag_gap_over_3000);
    out += F("\npauseAllActive: ");
    out += pause_all_diag_active ? F("1") : F("0");
    out += F("\npauseAllActiveMs: ");
    out += String(pause_all_diag_active ? (uint32_t)(millis() - pause_all_diag_started_ms) : 0UL);
    out += F("\npauseAllCount: ");
    out += String(pause_all_diag_count);
    out += F("\npauseAllLastDurationMs: ");
    out += String(pause_all_diag_last_duration_ms);
    out += F("\npauseAllMaxDurationMs: ");
    out += String(pause_all_diag_max_duration_ms);
                                                                               
                                                                             
                                                                              
                                   
    if (bwc && diagEntryHeap >= 6500UL && diagEntryBlock >= 3500UL) {
        String pumpDiag;
        pumpDiag.reserve(1024);
        bwc->getPumpDiag(pumpDiag);
        out += pumpDiag;
    } else if (bwc) {
        out += F("\npumpStateDiagDeferredLowHeap: 1");
    }
    out += F("\n\n--- Presence log (current) ---\n");
    diagAppendHtmlEscaped(out, presence_diag_log);
    out += F("\n--- Presence log (boot/persisted) ---\n");
    diagAppendHtmlEscaped(out, presence_diag_boot);

    out += F("</pre></body></html>");
    out.flush();
    server->chunkedResponseFinalize();
}


void handleGetSmartSchedule()
{
    if(!bwc) { server->send(503, F("text/plain"), F("Service Unavailable")); return; }
    String json; json.reserve(768); bwc->getJSONSmartSchedule(json);
    server->send(200, F("application/json"), json);
}

void handleSetSmartSchedule()
{
    if(!bwc) { server->send(503, F("text/plain"), F("Service Unavailable")); return; }
    StaticJsonDocument<256> doc;
    if(deserializeJson(doc, server->arg(0))) { server->send(400, F("text/plain"), F("Invalid JSON")); return; }
    uint64_t targetTime = doc[F("TARGETTIME")] | 0ULL;
    uint8_t targetTemp = doc[F("TARGETTEMP")] | 0;
    bool keepOn = doc[F("KEEPON")] | false;
    int poolCapacity = doc[F("POOLCAP")] | 0;
    if(bwc->setSmartSchedule(targetTime, targetTemp, keepOn, poolCapacity)) server->send(200, F("text/plain"), F("ok"));
    else server->send(400, F("text/plain"), F("Ungueltige Werte oder Uhrzeit nicht synchronisiert"));
}

void handleUpdateSmartSchedule()
{
    if(!bwc) { server->send(503, F("text/plain"), F("Service Unavailable")); return; }
    StaticJsonDocument<128> doc;
    if(deserializeJson(doc, server->arg(0)) || !doc.containsKey(F("KEEPON"))) { server->send(400, F("text/plain"), F("Invalid JSON")); return; }
    if(bwc->updateSmartScheduleKeepHeaterOn(doc[F("KEEPON")])) server->send(200, F("text/plain"), F("ok"));
    else server->send(404, F("text/plain"), F("Kein aktiver Zeitplan"));
}

void handleCancelSmartSchedule()
{
    if(!bwc) { server->send(503, F("text/plain"), F("Service Unavailable")); return; }
    bwc->cancelSmartSchedule(); server->send(200, F("text/plain"), F("ok"));
}

void handleGetConfig()
{
    if (!checkHttpPost(server->method())) return;

    String json;
    json.reserve(320);
    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }
    bwc->getJSONSettings(json);
    server->send(200, F("text/plain"), json);
}

   
                           
                                    
   
void handleSetConfig()
{
    if (!checkHttpPost(server->method())) return;

    String message = server->arg(0);
    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }
    bwc->setJSONSettings(message);

    server->send(200, F("text/plain"), "");
    send_mqtt_cfg_needed = true;
}

   
                             
                                    
   
void handleGetCommandQueue()
{
    if (!checkHttpPost(server->method())) return;

    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }
    String json = bwc->getJSONCommandQueue();
    server->send(200, F("application/json"), json);
}

   
                            
                             
   
void handleAddCommand()
{
                                                    

                                    
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message: ")+message);
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    if (bwc) bwc->add_command(item);

    server->send(200, F("text/plain"), F("ok"));
}

   
                             
                                                  
   
void handleEditCommand()
{
    if (!checkHttpPost(server->method())) return;

                                    
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    uint8_t index = doc[F("IDX")];
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }
    bwc->edit_command(index, item);

    server->send(200, F("text/plain"), "");
}

   
                            
                                                  
   
void handleDelCommand()
{
    if (!checkHttpPost(server->method())) return;

                                    
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    uint8_t index = doc[F("IDX")];
    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }
    bwc->del_command(index);

    server->send(200, F("text/plain"), "");
}

void handle_cmdq_file()
{
    if (!checkHttpPost(server->method())) return;

                                    
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    String action = doc[F("ACT")].as<String>();
    String filename = "/";
    filename += doc[F("NAME")].as<String>();

    if(action.equals("load"))
    {
        copyFile("/cmdq.json", "/cmdq.backup");
        copyFile(filename, "/cmdq.json");
        bwc->reloadCommandQueue();
    }
    if(action.equals("save"))
    {
        copyFile("/cmdq.json", filename);
    }

    server->send(200, F("text/plain"), "");
}

void copyFile(String source, String dest)
{
    char ibuffer[64];                    
    
    File f_source = LittleFS.open(source, "r");                              
    if (!f_source)
    {
        return;
    }

    File f_dest = LittleFS.open(dest, "w");                                    
    if (!f_dest)
    {
        return;
    }
    
    while (f_source.available() > 0)
    {
        byte i = f_source.readBytes(ibuffer, 64);                                                           
        f_dest.write(ibuffer, i);                                                          
    }
    
    f_dest.close();                                    
    f_source.close();                               
}

   
                                                             
   
void loadWebConfig()
{
                                     
    StaticJsonDocument<256> doc;

    File file = LittleFS.open(F("/webconfig.json"), "r");
    if (file)
    {
        DeserializationError error = deserializeJson(doc, file);
        if (error)
        {
                                                                     
        file.close();
        return;
        }
    }
    else
    {
                                                                               
    }

    showSectionTemperature = (doc.containsKey(F("SST")) ? doc[F("SST")] : true);
    showSectionDisplay = (doc.containsKey(F("SSD")) ? doc[F("SSD")] : true);
    showSectionControl = (doc.containsKey(F("SSC")) ? doc[F("SSC")] : true);
    showSectionButtons = (doc.containsKey(F("SSB")) ? doc[F("SSB")] : true);
    showSectionTimer = (doc.containsKey(F("SSTIM")) ? doc[F("SSTIM")] : true);
    showSectionTotals = (doc.containsKey(F("SSTOT")) ? doc[F("SSTOT")] : true);
    useControlSelector = (doc.containsKey(F("UCS")) ? doc[F("UCS")] : false);
}

   
                                                           
   
void saveWebConfig()
{
    File file = LittleFS.open(F("/webconfig.json"), "w");
    if (!file)
    {
                                                              
        return;
    }

                                    
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    if (serializeJson(doc, file) == 0)
    {
                                                                          
    }
    file.close();
}

   
                              
                                    
   
void handleGetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

                                    
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize webcfg\"}");
    }
    server->send(200, F("application/json"), json);
}

   
                              
                                    
   
void handleSetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

                                    
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
                                                           
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    showSectionTemperature = doc[F("SST")];
    showSectionDisplay = doc[F("SSD")];
    showSectionControl = doc[F("SSC")];
    showSectionButtons = doc[F("SSB")];
    showSectionTimer = doc[F("SSTIM")];
    showSectionTotals = doc[F("SSTOT")];
    useControlSelector = doc[F("UCS")];

    saveWebConfig();

    server->send(200, F("text/plain"), "");
}

   
                                                
   
void loadWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "r");
    if (!file)
    {
                                                                          
        return;
    }

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
                                                                
        file.close();
        return;
    }

    wifi_info.enableAp = doc[F("enableAp")];
    if(doc.containsKey(F("enableWM"))) wifi_info.enableWmApFallback = doc[F("enableWM")];
    wifi_info.apSsid = doc[F("apSsid")].as<String>();
    wifi_info.apPwd = doc[F("apPwd")].as<String>();

    wifi_info.enableStaticIp4 = doc[F("enableStaticIp4")];
    String s(30);
    wifi_info.ip4Address_str = doc[F("ip4Address")].as<String>();
    wifi_info.ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    wifi_info.ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    wifi_info.ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    wifi_info.ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    wifi_info.ip4NTP_str = doc[F("ip4NTP")].as<String>();

    return;

                                               
                                               
                                               
                                               
                                               
                                               
                                               
                                               
                                             
                                             
                                             
                                             
                                                     
                                                     
                                                     
                                                     
                                                         
                                                         
                                                         
                                                         
}

   
                                              
   
void saveWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "w");
    if (!file)
    {
                                                         
        return;
    }

    DynamicJsonDocument doc(1024);

    doc[F("enableAp")] = wifi_info.enableAp;
    doc[F("enableWM")] = wifi_info.enableWmApFallback;
    doc[F("apSsid")] = wifi_info.apSsid;
    doc[F("apPwd")] = wifi_info.apPwd;
    doc[F("enableStaticIp4")] = wifi_info.enableStaticIp4;
    doc[F("ip4Address")] = wifi_info.ip4Address_str;
    doc[F("ip4Gateway")] = wifi_info.ip4Gateway_str;
    doc[F("ip4Subnet")] = wifi_info.ip4Subnet_str;
    doc[F("ip4DnsPrimary")] = wifi_info.ip4DnsPrimary_str;
    doc[F("ip4DnsSecondary")] = wifi_info.ip4DnsSecondary_str;
    doc[F("ip4NTP")] = wifi_info.ip4NTP_str;

    if (serializeJson(doc, file) == 0)
    {
                                                                          
    }
    file.close();
}

   
                         
                                    
   


                                                                
                                                                                    
                                                                             
void handleWsState()
{
    if (!bwc) {
        server->send(503, F("text/plain"), F("Service Unavailable"));
        return;
    }

    wsstate_request_count++;
    const uint32_t fh = ESP.getFreeHeap();
    const uint32_t mb = ESP.getMaxFreeBlockSize();
                                                                                
                                                                            
    if (fh < 7000UL || mb < 4500UL) {
        wsstate_low_heap_defer_count++;
        server->sendHeader(F("Cache-Control"), F("no-store"));
        server->send(503, F("text/plain"), F("low heap - retry"));
        return;
    }

    server->sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
    if (!server->chunkedResponseModeStart(200, F("application/json"))) {
        server->send(505, F("text/plain"), F("HTTP/1.1 required"));
        return;
    }

    String part;
    part.reserve(768);
    server->sendContent("[");

    bwc->getJSONStates(part);
    server->sendContent(part);
    part.clear();
    serviceLocalHttpBackground();

    server->sendContent(",");
    bwc->getJSONTimes(part);
    server->sendContent(part);
    part.clear();
    serviceLocalHttpBackground();

    server->sendContent(",");
    getOtherInfo(part);
    server->sendContent(part);
    part.clear();
    serviceLocalHttpBackground();

    server->sendContent("]");
    server->chunkedResponseFinalize();
}

void handleGetWifi()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);

    doc[F("enableAp")] = wifi_info.enableAp;
    doc[F("enableWM")] = wifi_info.enableWmApFallback;
    doc[F("apSsid")] = wifi_info.apSsid;
    doc[F("apPwd")] = F("<enter password>");
    if (!hidePasswords)
    {
        doc[F("apPwd")] = wifi_info.apPwd;
    }

    doc[F("enableStaticIp4")] = wifi_info.enableStaticIp4;
    doc[F("ip4Address")] = wifi_info.ip4Address_str;
    doc[F("ip4Gateway")] = wifi_info.ip4Gateway_str;
    doc[F("ip4Subnet")] = wifi_info.ip4Subnet_str;
    doc[F("ip4DnsPrimary")] = wifi_info.ip4DnsPrimary_str;
    doc[F("ip4DnsSecondary")] = wifi_info.ip4DnsSecondary_str;
    doc[F("ip4NTP")] = wifi_info.ip4NTP_str;
    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("application/json"), json);
}

   
                         
                                    
   
void handleSetWifi()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
                                                           
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    wifi_info.enableAp = doc[F("enableAp")];
    if(doc.containsKey("enableWM")) wifi_info.enableWmApFallback = doc[F("enableWM")];
    wifi_info.apSsid = doc[F("apSsid")].as<String>();
    wifi_info.apPwd = doc[F("apPwd")].as<String>();

    wifi_info.enableStaticIp4 = doc[F("enableStaticIp4")];
    wifi_info.ip4Address_str = doc[F("ip4Address")].as<String>();
    wifi_info.ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    wifi_info.ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    wifi_info.ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    wifi_info.ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    wifi_info.ip4NTP_str = doc[F("ip4NTP")].as<String>();

    saveWifi();

    server->send(200, F("text/plain"), "");
}

  
                           
                                                                                  
                                                                                          
   
void handleResetWifi()
{
    if (!server) return;

                                                         
                                                      
    if (server->method() == HTTP_GET) {
        const String html =
            String(F("<html><head><meta charset='utf-8'>")) +
            F("<title>Reset WiFi</title></head><body>") +
            F("<h3>Reset WiFi (erase)</h3>") +
            F("<p>This will erase WiFi credentials and reboot the device.</p>") +
            F("<form method='POST' action='/resetwifi/'>") +
            F("<button type='submit'>Confirm WiFi reset</button>") +
            F("</form>") +
            F("<p><a href='/'>Cancel</a></p>") +
            F("</body></html>");
        server->send(200, F("text/html"), html);
        return;
    }

    if (server->method() != HTTP_POST) {
        server->send(405, F("text/plain"), F("Method Not Allowed"));
        return;
    }

    server->send(200, F("text/plain"), F("Resetting WiFi and rebooting..."));
    delay(200);

    const String r = String(F("HTTP /resetwifi ")) +
                     (server->method() == HTTP_POST ? F("POST") : F("OTHER")) +
                     String(F(" from ")) + server->client().remoteIP().toString() +
                     String(F(" uri=")) + server->uri();

    resetWiFi();
    requestRestart(r.c_str());
}

void resetWiFi()
{
    wifi_info.enableAp = false;
    wifi_info.enableWmApFallback = true;
    wifi_info.apSsid = F("empty");
    wifi_info.apPwd = F("empty");
    saveWifi();
    delay(3000);
    periodicTimer.detach();
    updateWSTimer.detach();
    if(ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    if (bwc) {
        bwc->saveSettings();
        bwc->stop();
    }
    delay(1000);
#if defined(ESP8266)
    ESP.eraseConfig();
#endif
    delay(1000);
    ESP_WiFiManager wm;
    wm.resetSettings();
                        
    delay(1000);
}

   
                                                
   
void loadMqtt()
{
    File file = LittleFS.open(F("/mqtt.json"), "r");
    if (!file)
    {
        Serial.println(F("Failed to read mqtt.json. Using defaults."));
        return;
    }

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
                                                                 
        file.close();
        return;
    }

    useMqtt = doc[F("enableMqtt")];
                                                                    

                                                 
    mqttCustomIpAddress[0] = doc[F("mqttIpAddress")][0];
    mqttCustomIpAddress[1] = doc[F("mqttIpAddress")][1];
    mqttCustomIpAddress[2] = doc[F("mqttIpAddress")][2];
    mqttCustomIpAddress[3] = doc[F("mqttIpAddress")][3];
    mqttCustomPort = doc[F("mqttPort")];
    mqttCustomUsername = doc[F("mqttUsername")].as<String>();
                                                                                
                                                                            
    if (doc.containsKey(F("mqttPassword"))) {
        String pw = doc[F("mqttPassword")].as<String>();
        if (pw.length() > 0) mqttCustomPassword = pw;
    }
    mqttCustomClientId = doc[F("mqttClientId")].as<String>();
    mqttCustomBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];
    if (doc.containsKey(F("mqttMode")))
    {
        String mode = doc[F("mqttMode")].as<String>();
        mqttCloudMode = mode.equalsIgnoreCase("cloud");
                                                    
                                                                             
                                                                               
                                                                            
                                                                            
                                                     
        if (mqttCloudMode && !SAR_CLOUD_V2_ALWAYS_ON) {
                                                                                                
            if (!enableMqtt) {
#if defined(ESP8266)
                if (mqttCloudMode) cloudTlsStopBounded(true, "mqtt-disabled-load");
                else {
                    if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
                    if (aWifiClient) aWifiClient->stop();
                }
#else
                if (mqttClient && mqttClient->connected()) mqttClient->disconnect();
#endif
                cloud_next_mqtt_try_ms = millis() + MQTT_BACKOFF_ON_STALL_MS;
                                     
                presence_next_poll_ms = millis() + 5000UL;
            }
                                                          
            if (enableMqtt) {
                presence_next_poll_ms = 0;
                cloud_next_mqtt_try_ms = 0;
            }
        }

    }

                                                           
                                                     
    mqttIpAddress = mqttCustomIpAddress;
    mqttPort      = mqttCustomPort;
    mqttUsername  = mqttCustomUsername;
    mqttPassword  = mqttCustomPassword;
    mqttClientId  = mqttCustomClientId;
    mqttBaseTopic = mqttCustomBaseTopic;

if (doc.containsKey(F("mqttPairingCode")))
{
    String newCode = doc[F("mqttPairingCode")].as<String>();
    newCode.trim();

    String oldCode = mqttPairingCode;
    oldCode.trim();

    mqttPairingCode = newCode;

                                                                               
    if (mqttCloudMode && newCode.length() >= 4 && newCode != oldCode)
    {
        cloud_pair_bootstrap_until_ms = millis() + 10000UL;               
        Serial.println(F("PAIRING: bootstrap MQTT enabled for 10s"));
        cloud_next_mqtt_try_ms = 0;                                        

                                                 
if (WiFi.status() == WL_CONNECTED && enableMqtt && mqttClient && !mqttClient->connected()) {
  Serial.println(F("PAIRING: bootstrap -> mqttConnect() now"));
  mqttConnect();
}

    }
}




                                                                                             
    if (doc.containsKey(F("mqttPairingSentHash"))) {
        mqttPairingSentHash = doc[F("mqttPairingSentHash")].as<String>();
        mqttPairingSentHash.trim();
        lastPairHash = mqttPairingSentHash;                      
    } else {
                                                                    
                                                                                           
                                                                         
        String code = mqttPairingCode; code.trim();
        if (mqttCloudMode && code.length() >= 4) {
            String payload = getMacClean() + ":" + code;
#if defined(ESP8266)
            mqttPairingSentHash = sha256Hex(payload);
#else
            mqttPairingSentHash = "";
#endif
            mqttPairingSentHash.trim();
            lastPairHash = mqttPairingSentHash;
            saveMqtt();                          
        }
    }

}



   
                                              
   
void saveMqtt()
{
    File file = LittleFS.open(F("/mqtt.json"), "w");
    if (!file)
    {
                                                         
        return;
    }

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = useMqtt;

                                                                              
    doc[F("mqttIpAddress")][0] = mqttCustomIpAddress[0];
    doc[F("mqttIpAddress")][1] = mqttCustomIpAddress[1];
    doc[F("mqttIpAddress")][2] = mqttCustomIpAddress[2];
    doc[F("mqttIpAddress")][3] = mqttCustomIpAddress[3];
    doc[F("mqttPort")]      = mqttCustomPort;
    doc[F("mqttUsername")]  = mqttCustomUsername;
    doc[F("mqttPassword")]  = mqttCustomPassword;
    doc[F("mqttClientId")]  = mqttCustomClientId;
    doc[F("mqttBaseTopic")] = mqttCustomBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqttTelemetryInterval;
    
    doc[F("mqttMode")] = mqttCloudMode ? "cloud" : "custom";
    doc[F("mqttPairingCode")] = mqttPairingCode;
    doc[F("mqttPairingSentHash")] = mqttPairingSentHash;



    if (serializeJson(doc, file) == 0)
    {
                                                                          
    }
    file.close();
}

   
                         
                                    
   
void handleGetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = useMqtt;

                                                                          
                                                                         
                                          
    doc[F("mqttIpAddress")][0] = mqttCustomIpAddress[0];
    doc[F("mqttIpAddress")][1] = mqttCustomIpAddress[1];
    doc[F("mqttIpAddress")][2] = mqttCustomIpAddress[2];
    doc[F("mqttIpAddress")][3] = mqttCustomIpAddress[3];
    doc[F("mqttPort")]      = mqttCustomPort;
    doc[F("mqttUsername")]  = mqttCustomUsername;

                                                                   
    doc[F("mqttPassword")]  = "<enter password>";

    doc[F("mqttClientId")]  = mqttCustomClientId;
    doc[F("mqttBaseTopic")] = mqttCustomBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqttTelemetryInterval;

    doc[F("mqttMode")] = mqttCloudMode ? "cloud" : "custom";
    doc[F("deviceMac")] = WiFi.macAddress();
    doc[F("deviceId")]  = getMacClean();
    doc[F("cloudV2Provisioned")] = cloudV2CredentialsProvisioned();
    doc[F("cloudV2Broker")] = SAR_CLOUD_HOST;
    doc[F("cloudV2Port")] = SAR_CLOUD_PORT;
    doc[F("mqttPairingCode")] = mqttPairingCode;
    doc[F("mqttPairingSentHash")] = mqttPairingSentHash;



    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("text/plain"), json);
}

   
                         
                                    
   
void handleSetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
                                                           
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

                                                                                              
                                            
                                           
useMqtt = (bool)doc[F("enableMqtt")];
enableMqtt = useMqtt;


    if (doc.containsKey(F("mqttMode")))
    {
        String mode = doc[F("mqttMode")].as<String>();
        mqttCloudMode = mode.equalsIgnoreCase("cloud");
    }

if (doc.containsKey(F("mqttPairingCode")))
{
    String newCode = doc[F("mqttPairingCode")].as<String>();
    newCode.trim();

    String oldCode = mqttPairingCode;
    oldCode.trim();

    mqttPairingCode = newCode;

    if (mqttCloudMode && newCode.length() >= 4 && newCode != oldCode)
    {
        cloud_pair_bootstrap_until_ms = millis() + 10000UL;               
        Serial.println(F("PAIRING: bootstrap MQTT enabled for 10s"));
        cloud_next_mqtt_try_ms = 0;                                        

                                                 
if (WiFi.status() == WL_CONNECTED && enableMqtt && mqttClient && !mqttClient->connected()) {
  Serial.println(F("PAIRING: bootstrap -> mqttConnect() now"));
  mqttConnect();
}

    }
}


                                                                 
                                                                                               
    if (!mqttCloudMode)
    {
        mqttCustomIpAddress[0] = doc[F("mqttIpAddress")][0];
        mqttCustomIpAddress[1] = doc[F("mqttIpAddress")][1];
        mqttCustomIpAddress[2] = doc[F("mqttIpAddress")][2];
        mqttCustomIpAddress[3] = doc[F("mqttIpAddress")][3];
        mqttCustomPort = doc[F("mqttPort")];
        mqttCustomUsername = doc[F("mqttUsername")].as<String>();
                                                                                    
                                                                            
    if (doc.containsKey(F("mqttPassword"))) {
        String pw = doc[F("mqttPassword")].as<String>();
        if (pw.length() > 0) mqttCustomPassword = pw;
    }
        mqttCustomClientId = doc[F("mqttClientId")].as<String>();
        mqttCustomBaseTopic = doc[F("mqttBaseTopic")].as<String>();

                                                                                   
        mqttIpAddress = mqttCustomIpAddress;
        mqttPort      = mqttCustomPort;
        mqttUsername  = mqttCustomUsername;
        mqttPassword  = mqttCustomPassword;
        mqttClientId  = mqttCustomClientId;
        mqttBaseTopic = mqttCustomBaseTopic;
    }

                                                            
    mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];



    server->send(200, F("text/plain"), "");
        


    saveMqtt();
    startMqtt();

                                                           
                                                                                      
    if (mqttClient && mqttClient->connected()) {
        publishPairingHash();
}

}

   
                     
                                    
   
void handleDir()
{
    HeapSelectIram ephemeral;
    Serial.printf_P(PSTR("dir IRamheap %d\n"), ESP.getFreeHeap());

    String mydir;
    mydir.reserve(128);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/html"), "");
    Dir root = LittleFS.openDir("/");
    while (root.next())
    {
                                                                            
        String rawName = root.fileName();
        if (rawName == F("sar_migration_state.json") ||
            rawName == F("/sar_migration_state.json")) {
            continue;
        }

                                           
        String href = root.fileName();
        if (href.endsWith(".gz")) href.remove(href.length()-3);
        mydir += F("<a href=\"/");
        mydir +=href;
        mydir += F("\">");
        mydir += root.fileName();
        mydir += F("</a>");
        mydir += F(" Size: ");
        mydir += String(root.fileSize());
        mydir += F(" Bytes ");
        mydir += F(" <a href=\"/remove/?FileToRemove=");
        mydir += root.fileName();
        mydir += F("\">remove</a><br>");
        server->sendContent(mydir);
        mydir.clear();
    }
    server->sendContent("");
}

   
                            
                                    
   
void handleFileUpload()
{
    HTTPUpload& upload = server->upload();
    String path;
    if (upload.status == UPLOAD_FILE_START)
    {
        path = upload.filename;
        if (!path.startsWith("/"))
        {
            path = "/" + path;
        }

                                                                        
        if (!path.endsWith(".gz"))
        {
                                                                                
            String pathWithGz = path + ".gz";
                                                                  
            if (LittleFS.exists(pathWithGz))
            {
                LittleFS.remove(pathWithGz);
            }
        }

        Serial.print(F("handleFileUpload Name: "));
        Serial.println(path);

                                                                             
        fsUploadFile = LittleFS.open(path, "w");
        path = String();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (fsUploadFile)
        {
                                                   
            fsUploadFile.write(upload.buf, upload.currentSize);
                                           
                                    
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        if (fsUploadFile)
        {
            fsUploadFile.close();
            Serial.print(F("handleFileUpload Size: "));
            Serial.println(upload.totalSize);
            server->sendHeader(F("location"), F("success.html"));
            server->send(303);
            if (upload.filename == "cmdq.json")
            {
                bwc->reloadCommandQueue();
            }
            if (upload.filename == "settings.json")
            {
                bwc->reloadSettings();
            }
        }
        else
        {
            Serial.println(F("err: 500"));
            server->send(500, F("text/plain"), F("500: couldn't create file"));
        }
    }
    else
    {
        Serial.print(F("upload status"));
        Serial.println(upload.status);
        server->send(500, F("text/plain"), F("500: upload aborted"));
    }
}

   
                            
                                  
   
void handleFileRemove()
{
    String path;
    path = server->arg(F("FileToRemove"));
    if (!path.startsWith("/"))
    {
        path = "/" + path;
    }

                                                  
                            

    if (LittleFS.exists(path) && LittleFS.remove(path))
    {
                                                         
                                
        if(server->method() == HTTP_GET)
            server->sendHeader(F("Location"), F("/dir/"));
        else
            server->sendHeader(F("Location"), F("/success.html"));
        server->send(303);
    }
    else
    {
                                                       
                                
        server->send(500, F("text/plain"), F("500: couldn't delete file"));
    }
}

   
                         
   
void handleRestart()
{
    if (!server) return;

                                                                                       
                                                      
    if (server->method() == HTTP_GET) {
        const String html =
            String(F("<html><head><meta charset='utf-8'>")) +
            F("<title>Restart</title></head><body>") +
            F("<h3>Restart device</h3>") +
            F("<form method='POST' action='/restart/'>") +
            F("<input type='hidden' name='bootid' value='") + bootIdString() + F("'>") +
            F("<button type='submit'>Confirm restart</button>") +
            F("</form>") +
            F("<p><a href='/'>Cancel</a></p>") +
            F("</body></html>");
        server->send(200, F("text/html"), html);
        return;
    }

    if (server->method() != HTTP_POST) {
        server->send(405, F("text/plain"), F("Method Not Allowed"));
        return;
    }

                                                                               
                                                                  
    if (!server->hasArg("bootid") || server->arg("bootid") != bootIdString()) {
        server->send(409, F("text/plain"), F("Restart confirmation expired. Open /restart/ again."));
        return;
    }

                                                                                          
    server->send(200, F("text/plain"), F("Restarting..."));
    delay(200);

    const String r = String(F("HTTP /restart POST from ")) +
                     server->client().remoteIP().toString() +
                     String(F(" uri=")) + server->uri();
    Serial.println(F("ESP restart (confirmed via /restart/ POST)"));
    requestRestart(r.c_str());
    delay(3000);
}

void updateStart(){
    Serial.println(F("update start"));
}
void updateEnd(){
    Serial.println(F("update finish"));
}
void udpateProgress(int cur, int total){
    Serial.printf_P(PSTR("update process at %d of %d bytes...\n"), cur, total);
}
void updateError(int err){
    Serial.printf_P(PSTR("update fatal error code %d\n"), err);
}

/** @author 877dev */



void startMqtt()
{
#if defined(ESP8266)
    if (!mqtt_stack_initialized) {
        mqtt_stack_initialized = true;
        mqtt_init_count++;
    } else {
        mqtt_reconfigure_count++;
    }
#endif
    Serial.printf_P(PSTR("DRAM heap before MQTT: %u\n"), ESP.getFreeHeap());
    {
        HeapSelectIram e;
        Serial.printf_P(PSTR("IRAM heap before MQTT: %u\n"), ESP.getFreeHeap());
    }

    Serial.println(F("startmqtt"));
#if defined(ESP8266)
    cloud_dns_preflight_valid_until_ms = 0;
    cloud_mqtt_stage_not_before_ms = 0;
#endif

                                                                         
    loadMqtt();

                                                                          
                                                                            
                                                                         
                                                                           
    if (mqttCloudMode) {
        enableMqtt = useMqtt;

                                                                           
                                                                     
        if (SAR_CLOUD_V2_ALWAYS_ON && enableMqtt && cloudV2CredentialsProvisioned()) {
            cloud_next_mqtt_try_ms = 0;
        }
    }

#if defined(ESP8266)
                        
    if (mqttCloudMode) {
                                                             
        tlsClient = &tlsClientStatic;
        tlsCa     = &mqttCloudCaRootYeStatic;

        tlsClient->setTrustAnchors(tlsCa);
                                                                                
                                                                             
                                                                      
        tlsClient->setSession(&mqttTlsSessionStatic);

                                  
        tlsClient->setBufferSizes(512, 512);

                                                                               
                                                                             
                                                              
        tlsClient->setTimeout(SAR_MQTT_TLS_CONNECT_TIMEOUT_MS);
        mqtt_tls_timeout_reapply_count++;
                                                                           
                                         
        tlsClient->setSSLVersion(BR_TLS12, BR_TLS12);

        aWifiClient = tlsClient;
    }
                                           
    else {
        aWifiClient = &wifiClientPlainStatic;
    }
#else
                                              
    aWifiClient = &wifiClientStatic;
#endif

mqttClient = &mqttClientStatic; mqttClient->setClient(*aWifiClient);

                                                   
                                                                                     
                                                                                                                
    if (!mqttClient->setBufferSize(mqttCloudMode ? 1024 : 2048)) {
        Serial.println(F("MQTT > WARNING: setBufferSize failed"));
    }


                                                                          
                                                                                 
                                  
#if defined(ESP8266)
    if (mqttCloudMode) cloudTlsStopBounded(true, "start-mqtt-reconfigure");
    else mqttClient->disconnect();
#else
    mqttClient->disconnect();
#endif

                              
String devId = getMacClean();

                                 
                                   
                                 
if (mqttCloudMode)
{
    mqttClientId  = devId;
    mqttBaseTopic = String(SAR_TOPIC_PREFIX) + devId;
    mqttUsername = devId;
    mqttPassword = cloudV2CredentialsSecret();
    mqttPort     = SAR_CLOUD_PORT;
    mqttClient->setServer(SAR_CLOUD_HOST, mqttPort);
    if (!cloudV2CredentialsProvisioned())
        Serial.println(F("[CloudV2] no device secret provisioned; MQTT connect disabled"));
}
else
{
                                                                     
    mqttIpAddress = mqttCustomIpAddress;
    mqttPort      = mqttCustomPort;
    mqttUsername  = mqttCustomUsername;
    mqttPassword  = mqttCustomPassword;
    mqttClientId  = mqttCustomClientId;
    mqttBaseTopic = mqttCustomBaseTopic;

    mqttClient->setServer(mqttIpAddress, mqttPort);

                                                                         
    if (mqttClientId.length() == 0) mqttClientId = devId;
}



    mqttClient->setKeepAlive(mqttCloudMode ? 60 : 60);
    mqttClient->setSocketTimeout(mqttCloudMode ? SAR_MQTT_CONNACK_TIMEOUT_S : 10);
#if defined(ESP8266)
    if (!mqttCloudMode) {
        wifiClientPlainStatic.setTimeout(10);
    }
#endif
    mqttClient->setCallback(mqttCallback);

    Serial.printf_P(PSTR("DRAM heap after MQTT init: %u\n"), ESP.getFreeHeap());
    {
        HeapSelectIram e;
        Serial.printf_P(PSTR("IRAM heap after MQTT init: %u\n"), ESP.getFreeHeap());
    }

                                                      
                                                                      
                                                                               
    if (!mqttCloudMode) {
        enableMqtt = useMqtt;                                                            
        custom_mqtt_kick = enableMqtt;                                                   
        custom_mqtt_kick_at_ms = millis();
        resetCustomHaDiscoverySchedule();
    }

}


                                                                                
                          
                                                                                
                                                                               
                                                                               
                                         

static void cloudV2SetAck(const char* op, bool ok, const String& rid = String(), const char* detail = nullptr)
{
    if (!mqttCloudMode) return;
    cloud_v2_ack.pending = true;
    cloud_v2_ack.ok = ok;
    snprintf(cloud_v2_ack.op, sizeof(cloud_v2_ack.op), "%s", op ? op : "");
    snprintf(cloud_v2_ack.rid, sizeof(cloud_v2_ack.rid), "%s", rid.c_str());
    snprintf(cloud_v2_ack.detail, sizeof(cloud_v2_ack.detail), "%s", detail ? detail : "");
}

static bool cloudV2PublishAck()
{
    if (!mqttCloudMode || !mqttClient || !mqttClient->connected() || !cloud_v2_ack.pending) return false;
    StaticJsonDocument<224> doc;
    doc[F("CONTENT")] = F("ACK");
    doc[F("OP")] = cloud_v2_ack.op;
    doc[F("OK")] = cloud_v2_ack.ok;
    if (cloud_v2_ack.rid[0]) doc[F("RID")] = cloud_v2_ack.rid;
    if (cloud_v2_ack.detail[0]) doc[F("DETAIL")] = cloud_v2_ack.detail;
    String json;
    json.reserve(192);
    if (serializeJson(doc, json) == 0) return false;
    if (!mqttPublishChecked(String(mqttBaseTopic) + F("/ack"), json, false)) return false;
    cloud_v2_ack.pending = false;
    return true;
}

static bool cloudV2PublishQueueState()
{
    if (!mqttCloudMode || !mqttClient || !mqttClient->connected() || !bwc) return false;
    String json = bwc->getJSONCommandQueue();
    return mqttPublishChecked(String(mqttBaseTopic) + F("/queue/state"), json, false);
}

static bool cloudV2PublishSmartScheduleState()
{
    if (!mqttCloudMode || !mqttClient || !mqttClient->connected() || !bwc) return false;
    String json;
    json.reserve(768);
    bwc->getJSONSmartSchedule(json);
    return mqttPublishChecked(String(mqttBaseTopic) + F("/smartschedule/state"), json, false);
}

static bool cloudV2PublishTimes()
{
    if (!mqttCloudMode || !mqttClient || !mqttClient->connected() || !bwc) return false;
    String json;
    json.reserve(512);
    bwc->getJSONTimes(json);
    return mqttPublishChecked(String(mqttBaseTopic) + F("/times"), json, false);
}

static void cloudV2ResponseTick()
{
    if (!mqttCloudMode || !mqttClient || !mqttClient->connected()) return;

                                                                            
                                                                                 
    if (cloud_v2_ack.pending) {
        (void)cloudV2PublishAck();
        return;
    }
    if (cloud_v2_publish_telemetry_pending) {
        cloud_v2_publish_telemetry_pending = false;
        sendMQTT();
        return;
    }
    if (cloud_v2_publish_times_pending) {
        cloud_v2_publish_times_pending = false;
        (void)cloudV2PublishTimes();
        return;
    }
    if (cloud_v2_publish_config_pending) {
        cloud_v2_publish_config_pending = false;
        sendMQTTConfig();
        return;
    }
    if (cloud_v2_publish_smartschedule_pending) {
        cloud_v2_publish_smartschedule_pending = false;
        (void)cloudV2PublishSmartScheduleState();
        return;
    }
    if (cloud_v2_publish_queue_pending) {
                                                                              
                                                                              
        cloud_v2_publish_queue_pending = false;
        (void)cloudV2PublishQueueState();
        return;
    }
}

/** @author 877dev */



void mqttCallback(char* topic, byte* payload, unsigned int length)
{
                                                                                 
    String message;
    message.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++)
        message += (char)payload[i];

    String t = String(topic);

                                                                            
                                                                            
                                                                          
                                                                            

                                     
    if (t.equals(String(mqttBaseTopic) + F("/command")))
    {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, message);
        if (error) {
            if (mqttCloudMode) cloudV2SetAck("command", false, String(), "invalid_json");
            return;
        }

        Commands command = doc[F("CMD")];
        int64_t value    = doc[F("VALUE")];
        int64_t xtime    = doc[F("XTIME")];
        int64_t interval = doc[F("INTERVAL")];
        String txt       = doc[F("TXT")] | "";
        String rid       = doc[F("RID")] | "";

        command_que_item item;
        item.cmd      = command;
        item.val      = value;
        item.xtime    = xtime;
        item.interval = interval;
        item.text     = txt;

        const bool ok = bwc ? bwc->add_command(item) : false;
        if (mqttCloudMode) {
            cloudV2SetAck("command", ok, rid, ok ? "accepted" : "service_unavailable");
            cloud_v2_publish_queue_pending = true;
        }
        return;
    }

                                           
    if (t.equals(String(mqttBaseTopic) + F("/command_batch")))
    {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, message);
        if (error) {
            if (mqttCloudMode) cloudV2SetAck("command_batch", false, String(), "invalid_json");
            return;
        }

        JsonArray commandArray = doc.as<JsonArray>();
        if (mqttCloudMode && commandArray.isNull()) {
            cloudV2SetAck("command_batch", false, String(), "invalid_json");
            return;
        }
        bool ok = (bwc != nullptr);
        unsigned int accepted = 0;

        for (JsonVariant commandItem : commandArray)
        {
            Commands command = commandItem[F("CMD")];
            int64_t value    = commandItem[F("VALUE")];
            int64_t xtime    = commandItem[F("XTIME")];
            int64_t interval = commandItem[F("INTERVAL")];
            String txt       = commandItem[F("TXT")] | "";

            command_que_item item;
            item.cmd      = command;
            item.val      = value;
            item.xtime    = xtime;
            item.interval = interval;
            item.text     = txt;

            if (bwc && bwc->add_command(item)) accepted++;
            else ok = false;
        }

        if (mqttCloudMode) {
                                                                             
                                                                      
            char detail[48];
            snprintf(detail, sizeof(detail), "accepted=%u", accepted);
            cloudV2SetAck("command_batch", ok, String(), detail);
            cloud_v2_publish_queue_pending = true;
        }
        return;
    }

                                        
    if (t.equals(String(mqttBaseTopic) + F("/set_config")))
    {
                                                                           
        if (bwc) {
            bwc->setJSONSettings(message);
            send_mqtt_cfg_needed = true;
        }
        if (mqttCloudMode) {
                                                                               
                                                                    
            cloudV2SetAck("set_config", bwc != nullptr, String(), bwc ? "accepted" : "service_unavailable");
            cloud_v2_publish_config_pending = true;
        }
        return;
    }

                                                                            
                                                                              
                                                                    
                                                                            
    if (!mqttCloudMode) return;

                                                               
    if (t.equals(String(mqttBaseTopic) + F("/smartschedule/set")))
    {
        StaticJsonDocument<320> doc;
        if (deserializeJson(doc, message)) {
            cloudV2SetAck("smartschedule/set", false, String(), "invalid_json");
            return;
        }
        const uint64_t targetTime = doc[F("TARGETTIME")] | 0ULL;
        const uint8_t targetTemp  = doc[F("TARGETTEMP")] | 0;
        const bool keepOn         = doc[F("KEEPON")] | false;
        const int poolCapacity    = doc[F("POOLCAP")] | 0;
        const String rid          = doc[F("RID")] | "";
        const bool ok = bwc && bwc->setSmartSchedule(targetTime, targetTemp, keepOn, poolCapacity);
        cloudV2SetAck("smartschedule/set", ok, rid, ok ? "accepted" : "invalid_values_or_time");
        cloud_v2_publish_smartschedule_pending = true;
        cloud_v2_publish_config_pending = true;                            
        return;
    }

                                                                  
    if (t.equals(String(mqttBaseTopic) + F("/smartschedule/update")))
    {
        StaticJsonDocument<160> doc;
        if (deserializeJson(doc, message) || !doc.containsKey(F("KEEPON"))) {
            cloudV2SetAck("smartschedule/update", false, String(), "invalid_json");
            return;
        }
        const String rid = doc[F("RID")] | "";
        const bool ok = bwc && bwc->updateSmartScheduleKeepHeaterOn(doc[F("KEEPON")]);
        cloudV2SetAck("smartschedule/update", ok, rid, ok ? "accepted" : "no_active_schedule");
        cloud_v2_publish_smartschedule_pending = true;
        return;
    }

                                                                  
    if (t.equals(String(mqttBaseTopic) + F("/smartschedule/cancel")))
    {
        StaticJsonDocument<96> doc;
        String rid;
        if (message.length() > 0 && !deserializeJson(doc, message)) rid = doc[F("RID")] | "";
        const bool ok = (bwc != nullptr);
        if (bwc) bwc->cancelSmartSchedule();
        cloudV2SetAck("smartschedule/cancel", ok, rid, ok ? "accepted" : "service_unavailable");
        cloud_v2_publish_smartschedule_pending = true;
        cloud_v2_publish_queue_pending = true;                                      
        return;
    }

                                                    
    if (t.equals(String(mqttBaseTopic) + F("/queue/get")))
    {
        StaticJsonDocument<96> doc;
        String rid;
        if (message.length() > 0 && !deserializeJson(doc, message)) rid = doc[F("RID")] | "";
        cloudV2SetAck("queue/get", bwc != nullptr, rid, bwc ? "accepted" : "service_unavailable");
        cloud_v2_publish_queue_pending = true;
        return;
    }

                                                    
    if (t.equals(String(mqttBaseTopic) + F("/queue/add")))
    {
        StaticJsonDocument<320> doc;
        if (deserializeJson(doc, message)) {
            cloudV2SetAck("queue/add", false, String(), "invalid_json");
            return;
        }
        command_que_item item;
        item.cmd      = (Commands)(doc[F("CMD")] | 0);
        item.val      = doc[F("VALUE")] | 0LL;
        item.xtime    = doc[F("XTIME")] | 0LL;
        item.interval = doc[F("INTERVAL")] | 0LL;
        String txt       = doc[F("TXT")] | "";
        item.text         = txt;
        const String rid  = doc[F("RID")] | "";
        const bool ok = bwc && bwc->add_command(item);
        cloudV2SetAck("queue/add", ok, rid, ok ? "accepted" : "service_unavailable");
        cloud_v2_publish_queue_pending = true;
        return;
    }

                                                     
    if (t.equals(String(mqttBaseTopic) + F("/queue/edit")))
    {
        StaticJsonDocument<320> doc;
        if (deserializeJson(doc, message) || !doc.containsKey(F("IDX"))) {
            cloudV2SetAck("queue/edit", false, String(), "invalid_json");
            return;
        }
        command_que_item item;
        item.cmd      = (Commands)(doc[F("CMD")] | 0);
        item.val      = doc[F("VALUE")] | 0LL;
        item.xtime    = doc[F("XTIME")] | 0LL;
        item.interval = doc[F("INTERVAL")] | 0LL;
        String txt       = doc[F("TXT")] | "";
        item.text         = txt;
        const uint8_t idx = doc[F("IDX")];
        const String rid = doc[F("RID")] | "";
        const bool ok = bwc && bwc->edit_command(idx, item);
        cloudV2SetAck("queue/edit", ok, rid, ok ? "accepted" : "invalid_index");
        cloud_v2_publish_queue_pending = true;
        return;
    }

                                                       
    if (t.equals(String(mqttBaseTopic) + F("/queue/delete")))
    {
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, message) || !doc.containsKey(F("IDX"))) {
            cloudV2SetAck("queue/delete", false, String(), "invalid_json");
            return;
        }
        const uint8_t idx = doc[F("IDX")];
        const String rid = doc[F("RID")] | "";
        const bool ok = bwc && bwc->del_command(idx);
        cloudV2SetAck("queue/delete", ok, rid, ok ? "accepted" : "invalid_index");
        cloud_v2_publish_queue_pending = true;
        return;
    }

                                                      
    if (t.equals(String(mqttBaseTopic) + F("/request_state")))
    {
        StaticJsonDocument<96> doc;
        String rid;
        if (message.length() > 0 && !deserializeJson(doc, message)) rid = doc[F("RID")] | "";
        cloudV2SetAck("request_state", bwc != nullptr, rid, bwc ? "accepted" : "service_unavailable");
        if (bwc) {
            cloud_v2_publish_telemetry_pending = true;                      
            cloud_v2_publish_times_pending = true;                                     
            cloud_v2_publish_config_pending = true;                   
            cloud_v2_publish_smartschedule_pending = true;
            cloud_v2_publish_queue_pending = true;
        }
        return;
    }
}


   
                                                                                   
   
void mqttConnect()
{
    if (!enableMqtt)
    {
        if (!(mqttCloudMode && !SAR_CLOUD_V2_ALWAYS_ON && (presence_allowed || cloudBootstrapActive()))) return;
    }
    if (mqttCloudMode && SAR_CLOUD_V2_ALWAYS_ON && !cloudV2CredentialsProvisioned()) {
        Serial.println(F("[CloudV2] MQTT connect skipped: device not provisioned"));
        return;
    }
    Serial.println(F("mqttconn"));
#if defined(ESP8266)
    mqtt_last_attempt_ms = millis();

                                                                             
                                                                             
                                                                          
    if (mqttCloudMode && cloud_mqtt_attempt_in_progress &&
        cloud_mqtt_stage_not_before_ms != 0) {
        const uint32_t nowStage = millis();
        if ((int32_t)(nowStage - cloud_mqtt_stage_not_before_ms) < 0) return;
        cloud_mqtt_stage_not_before_ms = 0;
    }

                                                                             
                                                                          
                                                                             
                                                                             
                                                                             
    if (mqttCloudMode && !cloud_mqtt_attempt_in_progress && (!tlsClient || !tlsClient->connected())) {
        uint32_t fh = ESP.getFreeHeap();
        uint32_t mb = ESP.getMaxFreeBlockSize();

                                                                             
                                                                               
                                                                              
                                                                              
                                                         
        if ((fh < SAR_TLS_ADMISSION_MIN_HEAP || mb < SAR_TLS_ADMISSION_MIN_BLOCK) &&
            mqttClient && !mqttClient->connected() && aWifiClient) {
            tls_reclaim_heap_before = fh;
            tls_reclaim_block_before = mb;
            cloudTlsStopBounded(false, "tls-admission-reclaim");
            yield();
            delay(0);
            fh = ESP.getFreeHeap();
            mb = ESP.getMaxFreeBlockSize();
            tls_reclaim_heap_after = fh;
            tls_reclaim_block_after = mb;
            tls_reclaim_count++;
        }

        tls_admission_last_heap = fh;
        tls_admission_last_block = mb;
        if (fh < SAR_TLS_ADMISSION_MIN_HEAP || mb < SAR_TLS_ADMISSION_MIN_BLOCK) {
            const uint32_t nowAdmission = millis();
            tls_admission_blocked_count++;
            tls_admission_last_blocked_ms = nowAdmission;
            if (tls_admission_blocked_since_ms == 0) tls_admission_blocked_since_ms = nowAdmission;
            g_mqtt_last_connect_ok = false;

            const uint32_t blockedAge = (uint32_t)(nowAdmission - tls_admission_blocked_since_ms);
            if (blockedAge >= SAR_TLS_ADMISSION_RECOVERY_MS) {
                tls_admission_recovery_restarts++;
                String reason = String(F("TLS_ADMISSION_STUCK fh=")) + fh +
                                F(" mb=") + mb +
                                F(" ageMs=") + blockedAge +
                                F(" blocks=") + tls_admission_blocked_count;
                Serial.println(String(F("[TLS] ")) + reason + F(" -> controlled restart"));
                cloudTlsStopBounded(false, "tls-admission-stuck");
                requestRestart(reason.c_str());
                return;
            }

            cloud_next_mqtt_try_ms = nowAdmission + SAR_TLS_ADMISSION_RETRY_MS;
            Serial.printf_P(PSTR("[TLS] admission blocked heap=%u block=%u age=%lu ms -> retry %lu ms\n"),
                            fh, mb, (unsigned long)blockedAge,
                            (unsigned long)SAR_TLS_ADMISSION_RETRY_MS);
            return;
        }
                                                                          
        tls_admission_blocked_since_ms = 0;
                                                                               
                                                                                  
        if (!timeLooksValid()) {
            tls_time_deferred_count++;
            g_mqtt_last_connect_ok = false;
            cloud_next_mqtt_try_ms = millis() + 5000UL;
            Serial.println(F("[TLS] system time not valid yet -> retry 5s"));
            return;
        }
    }

                                            
                                                   
                                                       
                                                                        
                                                
      
                                                                             
                                                                                
    if (mqttCloudMode && tlsClient && !tlsClient->connected()) {
        const uint32_t nowDns = millis();
        const bool dnsFresh = cloud_dns_preflight_valid_until_ms != 0 &&
                              (int32_t)(cloud_dns_preflight_valid_until_ms - nowDns) > 0;
        if (!dnsFresh) {
            if (!cloud_mqtt_attempt_in_progress) {
                cloud_mqtt_attempt_in_progress = true;
                mqtt_connect_attempt_count++;
            }

            IPAddress resolved;
            cloud_dns_preflight_attempt_count++;
            const uint32_t dnsStartedMs = millis();
            pause_cloud_tasks_only(true);
            yield(); delay(0);
            const bool dnsOk = WiFi.hostByName(SAR_CLOUD_HOST, resolved, SAR_CLOUD_DNS_STAGE_TIMEOUT_MS) == 1;
            pause_cloud_tasks_only(false);
            yield(); delay(0);
            cloud_dns_preflight_last_duration_ms = (uint32_t)(millis() - dnsStartedMs);
            if (cloud_dns_preflight_last_duration_ms > cloud_dns_preflight_max_duration_ms)
                cloud_dns_preflight_max_duration_ms = cloud_dns_preflight_last_duration_ms;

            if (!dnsOk || !resolved.isSet()) {
                cloud_dns_preflight_fail_count++;
                cloud_dns_preflight_valid_until_ms = 0;
                cloudMqttConnectFailure("dns-preflight");
                Serial.println(F("end mqttcon (DNS stage failed)"));
                return;
            }

            cloud_dns_preflight_success_count++;
            cloud_dns_preflight_ip = resolved;
            cloud_dns_preflight_valid_until_ms = millis() + SAR_CLOUD_DNS_CACHE_VALID_MS;
            cloud_tcp_stage_ready = false;
            cloud_mqtt_stage_not_before_ms = millis() + SAR_MQTT_STAGE_GAP_MS;
            cloud_next_mqtt_try_ms = cloud_mqtt_stage_not_before_ms;
            Serial.printf_P(PSTR("[TLS] DNS preflight OK ip=%s in %lu ms -> TCP stage next loop\n"),
                            resolved.toString().c_str(),
                            (unsigned long)cloud_dns_preflight_last_duration_ms);
            return;
        }

        if (!cloud_mqtt_attempt_in_progress) {
            cloud_mqtt_attempt_in_progress = true;
            mqtt_connect_attempt_count++;
        }

                                                                               
                                                                                
                                                                                
        if (!cloud_tcp_stage_ready) {
            cloudTlsStopBounded(false, "tcp-preconnect-clean");
            cloud_mqtt_attempt_in_progress = true;

            mqtt_tcp_preconnect_attempt_count++;
            mqtt_tcp_preconnect_heap_before = ESP.getFreeHeap();
            mqtt_tcp_preconnect_block_before = ESP.getMaxFreeBlockSize();
            const uint32_t tcpStartedMs = millis();

            pause_cloud_tasks_only(true);
            yield(); delay(0);
            const bool tcpOk = (tlsClientStatic.connectTcpOnly(cloud_dns_preflight_ip, mqttPort,
                                                               SAR_CLOUD_TCP_STAGE_TIMEOUT_MS) == 1);
            pause_cloud_tasks_only(false);
            yield(); delay(0);

            mqtt_tcp_preconnect_last_duration_ms = (uint32_t)(millis() - tcpStartedMs);
            if (mqtt_tcp_preconnect_last_duration_ms > mqtt_tcp_preconnect_max_duration_ms)
                mqtt_tcp_preconnect_max_duration_ms = mqtt_tcp_preconnect_last_duration_ms;
            mqtt_tcp_preconnect_heap_after = ESP.getFreeHeap();
            mqtt_tcp_preconnect_block_after = ESP.getMaxFreeBlockSize();

            if (!tcpOk || !tlsClientStatic.tcpConnectedOnly()) {
                mqtt_tcp_preconnect_fail_count++;
                cloudMqttConnectFailure("tcp-preconnect");
                Serial.println(F("end mqttcon (TCP stage failed)"));
                return;
            }

            mqtt_tcp_preconnect_success_count++;
            cloud_tcp_stage_ready = true;
            cloud_mqtt_attempt_in_progress = true;
            cloud_mqtt_stage_not_before_ms = millis() + SAR_MQTT_STAGE_GAP_MS;
            cloud_next_mqtt_try_ms = cloud_mqtt_stage_not_before_ms;
            Serial.printf_P(PSTR("[TLS] TCP preconnect OK in %lu ms -> TLS stage next loop\n"),
                            (unsigned long)mqtt_tcp_preconnect_last_duration_ms);
            return;
        }

        if (!tlsClientStatic.tcpConnectedOnly()) {
            cloud_tcp_stage_ready = false;
            cloudMqttConnectFailure("tcp-lost-before-tls");
            Serial.println(F("end mqttcon (TCP lost before TLS stage)"));
            return;
        }

                                                                          
                                                                             
                                                                             
                                                                   
        {
            const uint32_t preTlsHeap = ESP.getFreeHeap();
            const uint32_t preTlsBlock = ESP.getMaxFreeBlockSize();
            tls_admission_last_heap = preTlsHeap;
            tls_admission_last_block = preTlsBlock;
            if (preTlsHeap < SAR_TLS_ADMISSION_MIN_HEAP ||
                preTlsBlock < SAR_TLS_ADMISSION_MIN_BLOCK) {
                const uint32_t nowAdmission = millis();
                tls_admission_blocked_count++;
                tls_admission_last_blocked_ms = nowAdmission;
                if (tls_admission_blocked_since_ms == 0)
                    tls_admission_blocked_since_ms = nowAdmission;

                                                                              
                                                                        
                cloudTlsStopBounded(false, "pre-tls-admission");
                yield(); delay(0);

                const uint32_t reclaimedHeap = ESP.getFreeHeap();
                const uint32_t reclaimedBlock = ESP.getMaxFreeBlockSize();
                tls_admission_last_heap = reclaimedHeap;
                tls_admission_last_block = reclaimedBlock;

                const uint32_t blockedAge =
                    (uint32_t)(nowAdmission - tls_admission_blocked_since_ms);
                if (blockedAge >= SAR_TLS_ADMISSION_RECOVERY_MS) {
                    tls_admission_recovery_restarts++;
                    String reason = String(F("TLS_ADMISSION_STUCK_PREHANDSHAKE fh=")) +
                                    reclaimedHeap + F(" mb=") + reclaimedBlock +
                                    F(" ageMs=") + blockedAge +
                                    F(" blocks=") + tls_admission_blocked_count;
                    requestRestart(reason.c_str());
                    return;
                }

                cloud_next_mqtt_try_ms = nowAdmission + SAR_TLS_ADMISSION_RETRY_MS;
                Serial.printf_P(PSTR("[TLS] pre-handshake admission blocked heap=%u block=%u -> retry %lu ms\n"),
                                reclaimedHeap, reclaimedBlock,
                                (unsigned long)SAR_TLS_ADMISSION_RETRY_MS);
                return;
            }
        }

                                                                             
                                                                          
        tlsClient->setX509Time(time(nullptr));
        tlsClient->setTimeout(SAR_MQTT_TLS_CONNECT_TIMEOUT_MS);
        mqtt_tls_timeout_reapply_count++;

        mqtt_tls_preconnect_attempt_count++;
        mqtt_tls_preconnect_heap_before = ESP.getFreeHeap();
        mqtt_tls_preconnect_block_before = ESP.getMaxFreeBlockSize();
        const uint32_t tlsStartedMs = millis();

        pause_cloud_tasks_only(true);
        yield(); delay(0);
        const bool tlsOk = tlsClientStatic.startTlsOnly(SAR_CLOUD_HOST, SAR_MQTT_TLS_CONNECT_TIMEOUT_MS);
        pause_cloud_tasks_only(false);
        yield(); delay(0);
        cloud_tcp_stage_ready = false;

        mqtt_tls_preconnect_last_duration_ms = (uint32_t)(millis() - tlsStartedMs);
        if (mqtt_tls_preconnect_last_duration_ms > mqtt_tls_preconnect_max_duration_ms)
            mqtt_tls_preconnect_max_duration_ms = mqtt_tls_preconnect_last_duration_ms;
        mqtt_tls_preconnect_heap_after = ESP.getFreeHeap();
        mqtt_tls_preconnect_block_after = ESP.getMaxFreeBlockSize();
        if (mqtt_tls_preconnect_last_duration_ms >= SAR_MQTT_SLOW_CONNECT_MS) mqtt_tls_preconnect_slow_count++;
        if (mqtt_tls_preconnect_last_duration_ms >= 4000UL) mqtt_tls_preconnect_over4s_count++;

        if (!tlsOk) {
            mqtt_tls_preconnect_fail_count++;
            cloudMqttConnectFailure("tls-handshake");
            Serial.println(F("end mqttcon (TLS stage failed)"));
            return;
        }

        mqtt_tls_preconnect_success_count++;
        tlsClient->setTimeout(SAR_MQTT_TLS_RUNTIME_TIMEOUT_MS);
        mqtt_tls_timeout_reapply_count++;
        cloud_mqtt_stage_not_before_ms = millis() + SAR_MQTT_STAGE_GAP_MS;
        cloud_next_mqtt_try_ms = cloud_mqtt_stage_not_before_ms;
        Serial.printf_P(PSTR("[TLS] handshake OK in %lu ms -> MQTT stage next loop\n"),
                        (unsigned long)mqtt_tls_preconnect_last_duration_ms);
        return;
    }

                                                                                
                                                             
    if (!mqttCloudMode || !cloud_mqtt_attempt_in_progress) {
        mqtt_connect_attempt_count++;
        if (mqttCloudMode) cloud_mqtt_attempt_in_progress = true;
    }
    mqtt_last_connect_heap_before = ESP.getFreeHeap();
    mqtt_last_connect_block_before = ESP.getMaxFreeBlockSize();
    const uint32_t mqttConnectStartedMs = millis();

                                                            
                                                                         
    if (mqttCloudMode && tlsClient) tlsClient->setX509Time(time(nullptr));
#endif

                                                 
                                                                                                                          
#if defined(ESP8266)
    if (mqttCloudMode && tlsClient) {
                                                                              
                                                                                
                                                                             
        if (!tlsClient->connected()) {
            cloudMqttConnectFailure("tls-lost-before-mqtt");
            Serial.println(F("end mqttcon (TLS lost before MQTT stage)"));
            return;
        }
                                                                          
                                                                              
                                                                                
        tlsClient->setTimeout(SAR_MQTT_TLS_RUNTIME_TIMEOUT_MS);
        mqtt_tls_timeout_reapply_count++;
        mqttClient->setSocketTimeout(SAR_MQTT_CONNACK_TIMEOUT_S);
    }
#endif
                                                                             
                                                                            
    const bool doPause = mqttCloudMode;
    if (doPause) pause_cloud_tasks_only(true);
    yield(); delay(0);

    bool _mqtt_ok = mqttClient->connect(
        mqttClientId.c_str(),             
        mqttUsername.c_str(),                       
        mqttPassword.c_str(),                       
        (String(mqttBaseTopic) + F("/Status")).c_str(),             
        0,              
        true,              
        "Dead"               
    );

    
    g_mqtt_last_connect_ok = _mqtt_ok;
if (doPause) pause_cloud_tasks_only(false);
    yield(); delay(0);
#if defined(ESP8266)
    mqtt_last_connect_duration_ms = (uint32_t)(millis() - mqttConnectStartedMs);
    mqtt_last_connect_heap_after = ESP.getFreeHeap();
    mqtt_last_connect_block_after = ESP.getMaxFreeBlockSize();
    if (mqttCloudMode && mqtt_last_connect_duration_ms >= SAR_MQTT_SLOW_CONNECT_MS) mqtt_slow_connect_count++;
    if (mqttCloudMode && mqtt_last_connect_duration_ms >= 5000UL) mqtt_over_5s_connect_count++;
#endif

    if (_mqtt_ok)
    {
                                         
        mqtt_connect_count++;
#if defined(ESP8266)
        mqtt_last_connected_ms = millis();
        mqtt_last_connect_success_ms_diag = mqtt_last_connected_ms;
        mqtt_connect_success_count++;
        mqtt_fail_streak = 0;
        if (mqttCloudMode) {
            cloud_next_mqtt_try_ms = 0;
            tls_admission_blocked_since_ms = 0;
            cloud_mqtt_attempt_in_progress = false;
            cloud_mqtt_stage_not_before_ms = 0;
        }
#endif

if (mqttCloudMode && !SAR_CLOUD_V2_ALWAYS_ON)
{
    presence_polling_paused = false;
}



                                                                       
                                                                                           
if (!mqttCloudMode)
{
}



        

                                                                                                                                    
                                                                                                                                
const bool retainIdentity = !mqttCloudMode;                                
const bool retainStatus   = true;                                                                

if (mqttClient) {
    if (!mqttPublishChecked(String(mqttBaseTopic) + F("/Status"), "Alive", retainStatus)) return;
    if (!mqttPublishChecked(String(mqttBaseTopic) + F("/MAC_Address"), WiFi.macAddress().c_str(), retainIdentity)) return;
    if (!mqttPublishChecked(String(mqttBaseTopic) + F("/MQTT_Connect_Count"), String(mqtt_connect_count), retainIdentity)) return;
    mqttServiceTick();
}

        publishPairingHash();                    
        yield(); delay(0);
        if (mqttClient) mqttClient->loop();


                                                               
        if (mqttClient) {
            mqttClient->subscribe((String(mqttBaseTopic) + F("/command")).c_str());
            mqttServiceTick();
            mqttClient->subscribe((String(mqttBaseTopic) + F("/command_batch")).c_str());
            mqttServiceTick();
            mqttClient->subscribe((String(mqttBaseTopic) + F("/set_config")).c_str());
            mqttServiceTick();

                                                                               
                                                                    
            if (mqttCloudMode) {
                mqttClient->subscribe((String(mqttBaseTopic) + F("/smartschedule/set")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/smartschedule/update")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/smartschedule/cancel")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/queue/get")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/queue/add")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/queue/edit")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/queue/delete")).c_str());
                mqttServiceTick();
                mqttClient->subscribe((String(mqttBaseTopic) + F("/request_state")).c_str());
                mqttServiceTick();

                                                                            
                                                                             
                cloud_v2_publish_telemetry_pending = true;
                cloud_v2_publish_times_pending = true;
                cloud_v2_publish_config_pending = true;
                cloud_v2_publish_smartschedule_pending = true;
                cloud_v2_publish_queue_pending = true;
            }
        }

#ifdef ESP8266
                                                                                    
                                                                                    
    if (!mqttCloudMode)
    {
        armCustomHaDiscovery(1500UL);
        Serial.println(F("Custom HA discovery armed"));

                                                                 
                                                                                
                                                                                       
        uint32_t telemetryIntervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
        if (telemetryIntervalMs < 60000UL) telemetryIntervalMs = 60000UL;
        mqtt_telemetry_enabled = true;
        mqtt_next_telemetry_ms = millis() + telemetryIntervalMs;
    }
    else
    {
                                          
        mqtt_telemetry_enabled = false;
        mqtt_next_telemetry_ms = 0;
    }
#endif

    }
    else
    {
        if (!mqttCloudMode) {
            mqtt_telemetry_enabled = false;
            mqtt_next_telemetry_ms = 0;
        }
        Serial.print(F("MQTT connect FAILED, state="));
        Serial.println(mqttClient ? mqttClient->state() : 999);
#if defined(ESP8266)
        if (mqttCloudMode) {
                                                                                 
                                                                              
                                                                               
            cloudMqttConnectFailure("mqtt-handshake");
        } else {
            mqtt_connect_fail_count++;
            if (mqtt_fail_streak < 250) mqtt_fail_streak++;
        }
#endif

                                                                    
                                                                           
                                                                      
    }

    Serial.println(F("end mqttcon"));


}

time_t getBootTime()
{
    time_t seconds = millis() / 1000;
    time_t result = time(nullptr) - seconds;
    return result;
}

void handleESPInfo()
{
    #ifdef ESP8266
    char stack;
    uint32_t stacksize = stack_start - &stack;
    size_t const BUFSIZE = 1024;
    char response[BUFSIZE];

    char const *response_dram =
    PSTR(
    "Stack size:               %u \n"
    "Free Dram Heap:           %u \n"
    "Min Dram Heap:            %u \n"
    "Max free Dram block size: %u \n\n");

    char const *response_iram =
    PSTR(
    "Free Iram Heap:           %u \n"
    "Max free Iram block size: %u \n\n"
    "Core version:             %s \n"
    "CPU fq:                   %u MHz\n"
    "Cycle count:              %u \n"
    "Free cont stack:          %u \n"
    "Sketch size:              %u \n"
    "Free sketch space:        %u \n"
    );

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    snprintf_P(response, BUFSIZE, response_dram,
        stacksize,
        ESP.getFreeHeap(),
        heap_water_mark,
        ESP.getMaxFreeBlockSize() );
    server->sendContent(response);
    uint32_t iram_heap; 
    uint32_t iram_maxblock;
    {
        HeapSelectIram ephemeral;
        iram_heap = ESP.getFreeHeap();
        iram_maxblock = ESP.getMaxFreeBlockSize();
    }
    snprintf_P(response, BUFSIZE, response_iram,
        iram_heap,
        iram_maxblock,
        ESP.getCoreVersion().c_str(),
        ESP.getCpuFreqMHz(),
        ESP.getCycleCount(),
        ESP.getFreeContStack(),
        ESP.getSketchSize(),
        ESP.getFreeSketchSpace()
    );
    
    server->sendContent(response);
    server->sendContent("");

    Serial.println(F("end info"));
    #endif
}

void setTemperatureFromSensor()
{
    if(bwc->hasTempSensor)
    { 
            tempSensors->requestTemperatures(); 
            float temperatureC = tempSensors->getTempCByIndex(0);
                                                                  
                                         
                                   
                                         
                                   

                               
            if(temperatureC >= -20.0)
            {
                bwc->setAmbientTemperature(temperatureC, true);
            }
    }

                                                     
    if (mqtt_telemetry_enabled && mqttClient && mqttClient->connected()) {
        if (millis() >= mqtt_next_telemetry_ms) {
            uint32_t telemetryIntervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
            if (telemetryIntervalMs < 60000UL) telemetryIntervalMs = 60000UL;
            mqtt_next_telemetry_ms = millis() + telemetryIntervalMs;
            sendMQTTFlag = true;
        }
    }

}

#include "ha.txt"
#include "prometheus.txt"
