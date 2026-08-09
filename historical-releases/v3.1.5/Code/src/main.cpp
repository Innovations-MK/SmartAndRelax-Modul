#include "main.h"
#include "mqtt_ca.h"


// --- Boot diagnostics (shown in Web UI at /diag) ---
static String g_boot_diag;
static uint32_t g_boot_millis = 0;

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


// --- Restart marker (persisted for Web UI diagnostics) ---
static const char* kRestartMarkerPath = "/last_restart_marker.txt";
static String g_last_restart_marker_boot;

static void writeRestartMarker(const String& reason) {
    // Best effort: LittleFS should be mounted already. If not, this silently fails.
    File f = LittleFS.open(kRestartMarkerPath, "w");
    if (!f) return;

    // Include both millis() and time if available (NTP), but don't depend on it.
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
}

static void requestRestart(const char* reason) {
    // Reason might be null; keep it safe.
    String r = reason ? String(reason) : String("unknown");
    writeRestartMarker(r);
    delay(50);
    ESP.restart();
}



#if defined(ESP8266)
 #include <bearssl/bearssl.h>   // <-- DAS HINZUFÜGEN (für SHA256)

 // ------------------------------
 // Crash-Fix: statische TLS/MQTT Objekte (kein new/delete)
 // ------------------------------
 static BearSSL::WiFiClientSecure tlsClientStatic;
 static BearSSL::X509List         tlsCaStatic(SAR_MQTT_CA_CERT);

 // Legacy Pointer-Aliase (damit der restliche Code unverändert bleibt)
 BearSSL::WiFiClientSecure *tlsClient = &tlsClientStatic;
 BearSSL::X509List *tlsCa = &tlsCaStatic;
#endif

// Statischer MQTT-Client (auf TLS) – Pointer mqttClient bleibt kompatibel
#if defined(ESP8266)
 static PubSubClient mqttClientStatic(tlsClientStatic);
#else
 static WiFiClient   wifiClientStatic;
 static PubSubClient mqttClientStatic(wifiClientStatic);
#endif




static String getMacClean()
{
    String mac = WiFi.macAddress();   // "AA:BB:CC:DD:EE:FF"
    mac.replace(":", "");
    mac.replace("-", "");
    mac.toUpperCase();
    return mac;
}

static bool timeLooksValid() {
  time_t now = time(nullptr);
  return (now > 1700000000); // grob: >= 2023
}

static void waitForValidTime(uint32_t maxWaitMs = 8000) {
  uint32_t t0 = millis();
  while (!timeLooksValid() && (millis() - t0) < maxWaitMs) {
    delay(10);
    yield();
  }
}


// --------------------
// Cloud Presence Gate (Recommended)
// --------------------
static const uint32_t PRESENCE_POLL_OFFLINE_MS  = 3UL * 60UL * 1000UL;  // 3 Minuten
static const uint32_t PRESENCE_POLL_BURST_MS = 20000UL; // 20 Sekunden
static const uint32_t PRESENCE_POLL_ONLINE_MS = 60000UL; // 60 Sekunden
static const uint32_t PRESENCE_ACTIVE_WINDOW_MS = 3UL  * 60UL * 1000UL;  // 3 Minuten
static const uint32_t PRESENCE_GRACE_MS         = 45UL * 1000UL;         // 45 Sekunden

// -------- Presence Debug --------
// NUR auf true setzen, wenn du aktiv debuggen willst
static const bool PRESENCE_DEBUG = true;





static const char* PRESENCE_HOST = "europe-west3-smartandrelax-cloud.cloudfunctions.net";
static const uint16_t PRESENCE_PORT = 443;
static const char* PRESENCE_PATH_PREFIX = "/presence?deviceId=";

static uint32_t presence_active_until_ms = 0;

// Cloud Functions Host (NICHT setInsecure!)
#if defined(ESP8266)
static BearSSL::WiFiClientSecure presenceClient;
static BearSSL::X509List* presenceCa = nullptr;
static BearSSL::Session presenceSession;







static bool presenceTlsReady = false;

static void initPresenceTlsOnce()
{
  if (presenceTlsReady) return;

  if (!presenceCa) {
    presenceCa = new BearSSL::X509List(PRESENCE_ROOT_CA);
  }

  presenceClient.setTrustAnchors(presenceCa);   // nur 1x!
  presenceClient.setSession(&presenceSession);  // ✅ enables session resumption
  presenceClient.setBufferSizes(512, 512);
  presenceClient.setTimeout(5);

  // TLS braucht gültige Zeit
  waitForValidTime(8000);
  presenceClient.setX509Time(time(nullptr));

  presenceTlsReady = true;
}


#endif



static uint32_t presence_next_poll_ms = 0;
static uint32_t presence_grace_until_ms = 0;
static bool     presence_allowed = false;

// --- Pairing Bootstrap: MQTT kurz erlauben nach Pairing-Code Save ---
static uint32_t cloud_pair_bootstrap_until_ms = 0;
static uint32_t cloud_next_mqtt_try_ms = 0;   // <--- NEU

#if defined(ESP8266)
// --- MQTT Recovery / Health counters ---
static uint32_t mqtt_last_connected_ms = 0;
static uint32_t mqtt_last_attempt_ms   = 0;
static uint8_t  mqtt_fail_streak       = 0;
static uint32_t mqtt_stack_reset_ms    = 0;
#endif


static inline bool cloudBootstrapActive()
{
  return (int32_t)(cloud_pair_bootstrap_until_ms - millis()) > 0;
}


// --- Presence Poll Jitter (gegen Lastspitzen) ---
static uint32_t addJitter(uint32_t baseMs) {
  // ±10% Jitter
  int32_t j = (int32_t)(baseMs / 10);
  int32_t r = (int32_t)random(-j, j + 1);
  int32_t out = (int32_t)baseMs + r;
  if (out < 1000) out = 1000; // nie unter 1s
  return (uint32_t)out;
}

// --- Presence Keep-Alive Connection Management ---
static uint32_t presence_last_use_ms = 0;
static uint32_t presence_conn_open_ms = 0;

// wie lange darf eine TLS/TCP Verbindung maximal offen bleiben (Anti-Leak / Anti-Hang)
static const uint32_t PRESENCE_CONN_MAX_AGE_MS  = 120000UL;  // 2 Minuten

// wenn so lange nicht benutzt -> schließen (Server könnte sowieso droppen)
static const uint32_t PRESENCE_CONN_IDLE_CLOSE_MS = 30000UL; // 30 Sekunden


// Harte Bremse: wenn MQTT connected, Polling pausieren bis Disconnect
static bool presence_polling_paused = false;

// Cloud Telemetry scheduling (wie vorher, aber ohne Duty Window)
static uint32_t sar_cloud_next_telemetry_ms = 0;

static inline bool isPaired()
{
    String code = mqttPairingCode;
    code.trim();
    return code.length() >= 4; // gleiche Logik wie publishPairingHash()
}

static inline bool cloudPollingEnabled()
{
    // Presence-Polling muss IMMER laufen, unabhängig von MQTT enable/pairing.
    // MQTT folgt später der Presence-Entscheidung.
    return mqttCloudMode && (WiFi.status() == WL_CONNECTED);
}





// Minimaler HTTPS GET der 1 Byte ("1" oder "0") liefert.
// Returns: true wenn request erfolgreich (Response gelesen), false bei Fehlern (Timeout etc.)
static bool cloudPresenceFetch(bool &outAllowed)
{
#if !defined(ESP8266)
    (void)outAllowed;
    return false;
#else
    initPresenceTlsOnce();

        // --- PATCH: keep MQTT alive while waiting for HTTPS response ---
    auto pumpBackground = [&]() {
      if (mqttClient) mqttClient->loop();  // wichtig: MQTT weiter bedienen
      delay(0);
      yield();
    };


    // Optional: wenn Heap sehr niedrig ist, lieber fail-closed (verhindert Reboots)
if (ESP.getFreeHeap() < 12000 || ESP.getMaxFreeBlockSize() < 6000) {
  return false;
}


String devId = getMacClean();
String urlPath = String(PRESENCE_PATH_PREFIX) + devId;

if (PRESENCE_DEBUG) {
  Serial.printf_P(PSTR("PRESENCE REQ: https://%s%s (deviceId=%s)\n"),
                  PRESENCE_HOST, urlPath.c_str(), devId.c_str());
}

// --- Always fresh connect (no keep-alive) ---
if (presenceClient.connected()) {
  presenceClient.stop();
  yield();
}

if (!presenceClient.connect(PRESENCE_HOST, PRESENCE_PORT)) {
  char err[128];
  presenceClient.getLastSSLError(err, sizeof(err));
  if (PRESENCE_DEBUG) {
    Serial.printf_P(PSTR("PRESENCE TLS connect FAIL: %s\n"), err);
  }
  return false;
}



    // HTTP Request
presenceClient.print(
    String("GET ") + urlPath + " HTTP/1.1\r\n" +
    "Host: " + String(PRESENCE_HOST) + "\r\n" +
    "User-Agent: esp8266\r\n" +
    "Accept: text/plain\r\n" +
    "Connection: close\r\n\r\n"

);
presenceClient.flush(); // sicherstellen, dass Request raus ist



// --- Warten bis Response-Bytes da sind (sonst kommt statusLine manchmal leer) ---
uint32_t t_wait = millis();
while (presenceClient.connected() && !presenceClient.available() && (millis() - t_wait) < 800) {
    pumpBackground();
}


// Statusline lesen (mit Guard)
String statusLine;
if (presenceClient.available()) {
    statusLine = presenceClient.readStringUntil('\n');
    statusLine.trim();
}

if (PRESENCE_DEBUG) {
  Serial.printf_P(PSTR("PRESENCE HTTP status: '%s' avail=%d connected=%d\n"),
                  statusLine.c_str(),
                  (int)presenceClient.available(),
                  (int)presenceClient.connected());
}


if (statusLine.length() == 0) {
    // nichts bekommen -> Diagnose + fail
    char err[128];
    presenceClient.getLastSSLError(err, sizeof(err));
if (PRESENCE_DEBUG) {
  Serial.printf_P(PSTR("PRESENCE FAIL: empty statusline (SSL err: %s)\n"), err);
}
presenceClient.stop();
presence_conn_open_ms = 0;
presence_last_use_ms  = 0;
return false;

}


if (!(statusLine.startsWith("HTTP/1.1 2") || statusLine.startsWith("HTTP/1.0 2")))
{
    // Dump ein paar Header-Zeilen zur Diagnose (max 12)
if (PRESENCE_DEBUG) {
  for (int i = 0; i < 12 && presenceClient.connected(); i++)
  {
      String line = presenceClient.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
      line.trim();
      Serial.printf_P(PSTR("PRESENCE HDR: %s\n"), line.c_str());
      yield();
  }
} else {
  // wenn kein Debug: Header einfach skippen (wie du es sowieso schon machst)
}


presenceClient.stop();
presence_conn_open_ms = 0;
presence_last_use_ms  = 0;
return false;
}




// ---- Header lesen + Content-Length extrahieren ----
int contentLen = -1;
uint32_t t_hdr = millis();

while ((millis() - t_hdr) < 800)
{
    if (!presenceClient.available())
    {
        if (!presenceClient.connected()) break;
        pumpBackground();
        continue;
    }

    String line = presenceClient.readStringUntil('\n');
    line.trim(); // entfernt \r

    if (line.length() == 0) break; // leer = Header-Ende

    // Content-Length: 123
    if (line.startsWith("Content-Length:") || line.startsWith("content-length:"))
    {
        String v = line.substring(strlen("Content-Length:"));
        v.trim();
        contentLen = v.toInt();
    }

    pumpBackground();
}




// ---- Body vollständig lesen (Keep-Alive braucht "sauberen" Socket) ----
int firstNonWs = -1;
int bytesRead = 0;
uint32_t tBody = millis();

// Wenn Content-Length bekannt: exakt so viele Bytes lesen.
// Wenn unbekannt: wir lesen bis Timeout oder bis wir mind. 1 sinnvolles Byte haben.
while ((millis() - tBody) < 600)
{
    while (presenceClient.available())
    {
        int b = presenceClient.read();
        bytesRead++;

        // erstes nicht-Whitespace Byte merken
        if (firstNonWs < 0 && b >= 0 && b != '\r' && b != '\n' && b != ' ' && b != '\t')
        {
            firstNonWs = b;
        }

        // Wenn wir Content-Length kennen und komplett haben -> fertig
        if (contentLen >= 0 && bytesRead >= contentLen)
            goto body_done;
    }

    if (!presenceClient.connected())
        break;

    pumpBackground();
}

body_done:

if (firstNonWs < 0)
{
    if (PRESENCE_DEBUG) Serial.println(F("PRESENCE BODY: <no non-ws byte>"));
presenceClient.stop(); // bei Fehler lieber schließen
presence_conn_open_ms = 0;
presence_last_use_ms  = 0;
return false;

}

if (PRESENCE_DEBUG)
{
    Serial.printf_P(PSTR("PRESENCE BODY first byte: '%c' (%d) contentLen=%d bytesRead=%d\n"),
                    (char)firstNonWs, firstNonWs, contentLen, bytesRead);
}

outAllowed = (firstNonWs == '1');

// Wichtig: Presence TLS immer schließen, damit MQTT TLS genug RAM hat
presenceClient.stop();
presence_conn_open_ms = 0;
presence_last_use_ms  = 0;

return true;




#endif
}

// --- CLOUD DEBUG helper (wirkt nur im Cloud-Mode) ---
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



static void updatePresenceGate()
{
  uint32_t now = millis();

  if (!cloudPollingEnabled())
  {
    // Nicht "abschalten" (next_poll_ms=0) -> sonst kann Polling nach kurzen Aussetzern tot bleiben.
    // Stattdessen: Presence als nicht erlaubt markieren und in wenigen Sekunden erneut versuchen.
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

  // nicht fällig -> raus
  if (presence_next_poll_ms != 0 && (int32_t)(now - presence_next_poll_ms) < 0) {
    return;
  }

#if defined(ESP8266)
  // Low-heap: NICHT presence abschießen, nur später nochmal versuchen
  if (ESP.getFreeHeap() < 12000 || ESP.getMaxFreeBlockSize() < 6000) {
    presence_next_poll_ms = now + addJitter(60000UL);
    return;
  }
#endif

  bool allowed = false;
  bool ok = cloudPresenceFetch(allowed);

  if (PRESENCE_DEBUG) {
    Serial.printf_P(PSTR("PRESENCE > ok=%d allowed=%d heap=%u\n"),
                    ok ? 1 : 0, allowed ? 1 : 0, ESP.getFreeHeap());
  }

  if (ok) {
    presence_allowed = allowed;

    // Debug-Helfer: nur anzeigen, nicht als Logik nutzen
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

  // Fetch-Fehler: fail-open (Presence bleibt wie sie ist)
  // (du willst ja nicht bei TLS-Jitter sofort offline gehen)
  presence_next_poll_ms = now + addJitter(PRESENCE_POLL_BURST_MS);

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

    // Guard: nur bei Änderung publishen
    if (hash == lastPairHash) return;
    lastPairHash = hash;

    String topic = String(mqttBaseTopic) + "/pairing/hash";
    mqttClient->publish(topic.c_str(), hash.c_str(), true);

    Serial.print(F("PAIRING > published NEW retained hash to "));
    Serial.println(topic);
}


static void publishStatusRetained(const char* status)
{
    if (!mqttClient || !mqttClient->connected()) return;

    mqttClient->publish((String(mqttBaseTopic) + F("/Status")).c_str(), status, true);

    // kurz pumpen, damit das Publish wirklich rausgeht, bevor du disconnectest
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 150) {
        mqttClient->loop();
        delay(0);  // yield für ESP8266
    }
}


#if defined(ESP8266)
static void hardResetMqttStack(const char* reason)
{
    Serial.printf_P(PSTR("MQTT HARD RESET: %s\n"), reason ? reason : "(no reason)");

    // Stop timers that might use mqttClient while we rebuild it
    if (updateMqttTimer.active()) updateMqttTimer.detach();

    // Crash-Fix: keine delete/new mehr (Heap-Frag/Use-After-Free vermeiden)
    if (mqttClient) {
        if (mqttClient->connected()) mqttClient->disconnect();
    }
    if (tlsClient) {
        tlsClient->stop();
    }
    // Pointer bleiben gültig (statische Objekte)
    tlsClient   = &tlsClientStatic;
    tlsCa       = &tlsCaStatic;
    aWifiClient = tlsClient;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);


    // Presence socket also closed (fresh connect anyway)
    presenceClient.stop();

    // Re-init MQTT stack
    startMqtt();

    // Allow immediate reconnect attempt in Cloud loop
    cloud_next_mqtt_try_ms = 0;
    mqtt_fail_streak = 0;
    mqtt_stack_reset_ms = millis();
}

static void cloudMqttSupervisorTick()
{
    if (!mqttCloudMode) return;
    if (!enableMqtt) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (!isPaired()) return;

    // only supervise when we actually want MQTT online
    const bool shouldRun = presence_allowed || cloudBootstrapActive();
    if (!shouldRun) return;

    const uint32_t now = millis();

    // If mqttClient got nulled (after a stopall or error), rebuild it
    if (!mqttClient) {
        // avoid tight loops
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

    // Stuck heuristic: many consecutive failures or too long without a successful connection
    const bool tooLongOffline = (mqtt_last_connected_ms != 0) && ((uint32_t)(now - mqtt_last_connected_ms) > 10UL*60UL*1000UL);
    const bool manyFails      = (mqtt_fail_streak >= 12); // ~12 failed attempts (with 5s retry ~= 1min)

    // Rate-limit hard resets
    if ((tooLongOffline || manyFails) && ((uint32_t)(now - mqtt_stack_reset_ms) > 120000UL)) {
        hardResetMqttStack(tooLongOffline ? "offline>10min" : "many connect fails");
    }
}
#endif





// initial stack
char *stack_start;
uint32_t heap_water_mark;

// Setup a oneWire instance to communicate with any OneWire devices
// Setting arbitrarily to 231 since this isn't an actual pin
// Later during "setup" the correct pin will be set, if enabled 
OneWire *oneWire;
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature *tempSensors;

WiFiEventHandler gotIpEventHandler, disconnectedEventHandler;
void cb_gotIP(const WiFiEventStationModeGotIP& event)
{
    Serial.print("got IP: ");
    Serial.println(WiFi.localIP());

    startNTP();
    startOTA();
    startHttpServer();
    startWebSocket();
    startMqtt();
}

void cb_disconnected(const WiFiEventStationModeDisconnected& event)
{
    Serial.println(F("disconnected"));

    // --- Cloud Recovery: reset gating + close sockets on WiFi drop ---
    presence_allowed = false;
    presence_next_poll_ms = 0;
    presence_active_until_ms = 0;
    presence_grace_until_ms  = 0;
    cloud_next_mqtt_try_ms = 0;

#if defined(ESP8266)
    // close MQTT/TLS sockets to avoid stuck sessions
    if (mqttClient && mqttClient->connected()) {
        publishStatusRetained("Asleep");
        mqttClient->disconnect();
    }
    if (aWifiClient) aWifiClient->stop();
    presenceClient.stop();
#endif
}


void setup()
{
    
    // init record of stack
    char stack;
    stack_start = &stack;

    Serial.begin(115200);
    randomSeed(ESP.getChipId() ^ micros());
    Serial.println();
    Serial.println(F("[BOOT] ResetInfo:"));
    Serial.println(ESP.getResetInfo());
    Serial.printf_P(PSTR("[BOOT] Heap=%u maxBlock=%u frag=%u%%\n"),
                    ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation());
    
    // Capture boot diagnostics for Web UI (/diag)
    g_boot_millis = millis();
    {
        String tmp;
        tmp.reserve(512);
        tmp += F("ResetReason: ");
        tmp += ESP.getResetReason();
        tmp += F("\n\nResetInfo:\n");
        tmp += ESP.getResetInfo();
        tmp += F("\n\nLastRestartMarker:\n");
        if (g_last_restart_marker_boot.length()) tmp += g_last_restart_marker_boot; else tmp += F("(none)");

        tmp += F("\n\nBootHeap: ");
        tmp += String(ESP.getFreeHeap());
        tmp += F("  maxBlock: ");
        tmp += String(ESP.getMaxFreeBlockSize());
        tmp += F("  frag: ");
        tmp += String(ESP.getHeapFragmentation());
        tmp += F("%\nChipID: ");
        tmp += String(ESP.getChipId(), HEX);
        tmp += F("\nSDK: ");
        tmp += ESP.getSdkVersion();
        g_boot_diag = tmp;
    }

BWC_LOG_P(PSTR("\nStart\n"),0);
    BWC_LOG_P(PSTR("Millis: %d @ line: %d\n"), millis(), __LINE__);
    /*register wifi events */
    gotIpEventHandler = WiFi.onStationModeGotIP(cb_gotIP);
    disconnectedEventHandler = WiFi.onStationModeDisconnected(cb_disconnected);

    LittleFS.begin();
    loadRestartMarkerBoot();
    {
        HeapSelectIram ephemeral;
        // Serial.printf_P(PSTR("IRamheap %d\n"), ESP.getFreeHeap());
        bwc = new BWC;
        oneWire = new OneWire(231);
        tempSensors = new DallasTemperature(oneWire);
    }
    bwc->setup();
    bwc->loop();
    periodicTimer.attach(periodicTimerInterval, []{ periodicTimerFlag = true; });
    // update webpage every 2 seconds. (will also be updated on state changes)
    updateWSTimer.attach(2.0, []{ sendWSFlag = true; });
    loadWebConfig();
    // delayed mqtt start
    // Cloud-Mode: MQTT darf direkt (Presence regelt sowieso), Custom: kann verzögert bleiben
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
    bwc->print("   ");  //No overloaded function exists for the F() macro
    bwc->print(WiFi.localIP().toString());
    bwc->print("   ");
    bwc->print(FW_VERSION);
    Serial.println(F("End of setup()"));
    BWC_LOG_P(PSTR("Millis: %d @ line: %d\n"), millis(), __LINE__);
    heap_water_mark = ESP.getFreeHeap();
    Serial.println(ESP.getFreeHeap());
}

void loop()
{
    uint32_t freeheap = ESP.getFreeHeap();
    if(freeheap < heap_water_mark) heap_water_mark = freeheap;

    // We need this self-destructing info several times, so save it on the stack
    bool newData = bwc->newData();
    // Fiddle with the pump computer
    bwc->loop();

    // run only when a wifi connection is established
    if (WiFi.status() == WL_CONNECTED)
    {
        // listen for webserver events
        server->handleClient();

        // listen for OTA events
        ArduinoOTA.handle();

        // --- Cloud Presence Boot-Rearm (wichtig nach Power-Cycle) ---
        // Sobald WiFi verbunden ist, erzwingen wir einmalig einen sofortigen Presence-Poll.
        // Dadurch hängt das Polling NICHT vom Webinterface ab.
        static bool presenceBootArmed = false;
        static uint32_t presenceBootArmStartMs = 0;
        if (presenceBootArmStartMs == 0) presenceBootArmStartMs = millis();

        if (mqttCloudMode && useMqtt && !presenceBootArmed) {
            if (WiFi.status() == WL_CONNECTED) {
                presenceBootArmed = true;
                presence_next_poll_ms = millis(); // sofort fällig
                if (PRESENCE_DEBUG) Serial.println(F("[CLOUD] Boot-Rearm: forcing presence poll now"));
            } else if ((uint32_t)(millis() - presenceBootArmStartMs) > 30000UL) {
                // Fallback: auch wenn WL_CONNECTED nie kommt, trotzdem rearm (verhindert "tot")
                presenceBootArmed = true;
                presence_next_poll_ms = millis();
                if (PRESENCE_DEBUG) Serial.println(F("[CLOUD] Boot-Rearm fallback: forcing presence poll now"));
            }
        }

        // --- Cloud Presence Tick: immer laufen lassen (unabhängig von enableMqtt/WebUI) ---
        if (mqttCloudMode && useMqtt) {
            static uint32_t nextGateTickMs = 0;
            if ((int32_t)(millis() - nextGateTickMs) >= 0) {
                nextGateTickMs = millis() + 2000UL; // 2s
                updatePresenceGate();
            }
        }

        // Regelmäßiges Yield, damit WiFi/Webserver nicht verhungern
        delay(0);

        // Wenn Cloud/MQTT in der Config deaktiviert ist: Presence-Gate hart aus (kein Polling, kein Cloud-MQTT)
        if (mqttCloudMode && !useMqtt) {
            presence_allowed = false;
            presence_next_poll_ms = 0;
            presence_grace_until_ms = 0;
        }



// MQTT
if (mqttCloudMode || enableMqtt)
{
// -----------------------------
// Cloud Mode: Presence Gate wird jetzt unabhängig von enableMqtt weiter oben getickt.
// -----------------------------
// Debug nur wenn PRESENCE_DEBUG=true
if (PRESENCE_DEBUG) {
  static uint32_t dbgT = 0;
  if ((uint32_t)(millis() - dbgT) > 2000UL) {
    dbgT = millis();
    dbgCloudState("CLOUD");
  }
}



    // 2) Wenn Presence erlaubt: MQTT normal betreiben
    if (presence_allowed || cloudBootstrapActive())

    {
// Presence erlaubt => im Cloud-Mode MUSS MQTT laufen (mit Retry), ohne HA/Custom anzufassen
// Cloud: MQTT nur betreiben wenn Presence erlaubt ODER Bootstrap aktiv
bool mqttShouldRun = presence_allowed || cloudBootstrapActive();

if (mqttClient) mqttClient->loop();

if (mqttShouldRun)
{
    if (mqttClient && !mqttClient->connected())
    {
        if ((int32_t)(millis() - cloud_next_mqtt_try_ms) >= 0)
        {
            cloud_next_mqtt_try_ms = millis() + 5000UL;

            Serial.printf_P(PSTR("CLOUD: mqttConnect() (pres=%d boot=%d)\n"),
                            presence_allowed ? 1 : 0,
                            cloudBootstrapActive() ? 1 : 0);

            mqttConnect();
        }
    }
}
else
{
    // Presence nicht erlaubt und Bootstrap aus -> MQTT sauber offline halten
    if (mqttClient && mqttClient->connected())
    {
        publishStatusRetained("Asleep");
        mqttClient->disconnect();
    }
    if (aWifiClient) aWifiClient->stop();   // Socket immer zu
}



        // Telemetry-Interval im Cloud-Mode wie bisher über millis steuern
        if (mqttClient && mqttClient->connected())
        {
            uint32_t nowMs = millis();
            uint32_t intervalMs = (uint32_t)mqttTelemetryInterval * 1000UL;
            if (intervalMs > 0 && (int32_t)(nowMs - sar_cloud_next_telemetry_ms) >= 0)
            {
                sar_cloud_next_telemetry_ms = nowMs + intervalMs;
                sendMQTTFlag = true;
            }

            // Button publish nur wenn changed (Cloud: nicht retained)
            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);
            if (!msg.equals(prevButtonName))
            {
                mqttClient->publish((String(mqttBaseTopic) + F("/button")).c_str(), msg.c_str(), false);
                prevButtonName = msg;
            }

            if (newData || sendMQTTFlag)
            {
                sendMQTT();
                sendMQTTFlag = false;
            }

            if (send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                sendMQTTConfig();
            }
        }
    }
    else
    {
        // 3) Presence nicht erlaubt -> MQTT sauber offline halten
        if (mqttClient && mqttClient->connected())
        {
publishStatusRetained("Asleep");
mqttClient->disconnect();
if (aWifiClient) aWifiClient->stop();   // ✅ sauber TLS socket zu
        }
    }
}

    // --------------------------------
    // Custom/HomeAssistant: unverändert
    // --------------------------------
    else
    {
        if (mqttClient->loop())
        {
            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);

            // Home/Custom: retained wie gewohnt
            if (!msg.equals(prevButtonName))
            {
                const bool retainButton = true;
                mqttClient->publish((String(mqttBaseTopic) + F("/button")).c_str(), msg.c_str(), retainButton);
                prevButtonName = msg;
            }
            
            if (newData || sendMQTTFlag)
            {
                sendMQTT();
                sendMQTTFlag = false;
            }

            if (send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                sendMQTTConfig();
            }
        }
    }

#if defined(ESP8266)
        cloudMqttSupervisorTick();
#endif

        // web socket
        if (newData || sendWSFlag)
        {
            sendWSFlag = false;
            sendWS();
        }
    }

    // run every X seconds
    if (periodicTimerFlag)
    {
        periodicTimerFlag = false;
        if (WiFi.status() != WL_CONNECTED)
        {
            bwc->print(F("check network"));
            // Serial.println(F("WiFi > Trying to reconnect ..."));
        }
if (WiFi.status() == WL_CONNECTED)
{
    // ✅ Im Cloud-Mode NICHT dauerhaft reconnecten (Duty-Cycle macht das)
    if (enableMqtt && !mqttCloudMode)
    {
        if (!mqttClient->loop())
        {
            mqttConnect();
        }
    }
}

        // Leverage the pre-existing periodicTimerFlag to also set temperature, if enabled
        setTemperatureFromSensor();

        /* Debug */
        // static uint8_t minutes = 0;
        // minutes++;
        // if(minutes >= 5)
        // {
            // minutes = 0;
            // write_mem_stats_to_file();
        // }
    }

    if(checkNTP_flag)
    {
        checkNTP_flag = false;
        checkNTP();
    }

    if(CheckWiFi_flag)
    {
        CheckWiFi_flag = false;
        checkWiFi();
    }
    //Only do this if locked out! (by pressing POWER - LOCK - TIMER - POWER)
// Debounced: require the sequence to be stable for 2s to avoid accidental resets due to noise.
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
    //handleAUX();
    // static int temp_counter = 0;
    // if(++temp_counter % 100 == 0) BWC_LOG_P(PSTR("main loop %d\n"), millis());
}

/* Debugging to file, normally not used */
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
    /*Dram*/
    file.printf_P(PSTR("DRam: free %d, frag %d, max block %d\n"),
        ESP.getFreeHeap(), 
        ESP.getHeapFragmentation(),
        ESP.getMaxFreeBlockSize()
        );
    file.close();
}
    


/**
 * Send status data to web client in JSON format (because it is easy to decode on the other side)
 */
void sendWS()
{
    if(webSocket->connectedClients() == 0) return;
    HeapSelectIram ephemeral;
    // Serial.printf("IRamheap %d\n", ESP.getFreeHeap());
    // send states
    String json;
    json.reserve(384);

    bwc->getJSONStates(json);
    webSocket->broadcastTXT(json);
    // send times
    json.clear();
    bwc->getJSONTimes(json);
    webSocket->broadcastTXT(json);
    // send other info
    json.clear();
    getOtherInfo(json);
    webSocket->broadcastTXT(json);
    // json = bwc->getDebugData();
    // webSocket->broadcastTXT(json);
    // time_t now = time(nullptr);
    // struct tm timeinfo;
    // gmtime_r(&now, &timeinfo);
    // Serial.print("Current time: ");
    // Serial.print(asctime(&timeinfo));
}

void getOtherInfo(String &rtn)
{
    // DynamicJsonDocument doc(512);
    StaticJsonDocument<512> doc;
    // Set the values in the document
    doc[F("CONTENT")] = F("OTHER");
    doc[F("MQTT")] = mqttClient->state();
    /*TODO: add these:*/
    //   doc[F("PressedButton")] = bwc->getPressedButton();
    doc[F("HASJETS")] = bwc->hasjets;
    doc[F("HASGOD")] = bwc->hasgod;
    doc[F("MODEL")] = bwc->getModel();
    doc[F("RSSI")] = WiFi.RSSI();
    doc[F("IP")] = WiFi.localIP().toString();
    doc[F("SSID")] = WiFi.SSID();
    doc[F("FW")] = FW_VERSION;
    doc[F("loopfq")] = bwc->loop_count;
    bwc->loop_count = 0;

    // Serialize JSON to string
    if (serializeJson(doc, rtn) == 0)
    {
        rtn = F("{\"error\": \"Failed to serialize other\"}");
    }
}

/**
 * Send STATES and TIMES to MQTT
 * It would be more elegant to send both states and times on the "message" topic
 * and use the "CONTENT" field to distinguish between them
 * but it might break peoples home automation setups, so to keep it backwards
 * compatible I choose to start a new topic "/times"
 * @author 877dev
 */
void sendMQTT()
{
    String json;
    json.reserve(320);

    // ✅ Telemetry retained nur im Custom/HomeAssistant Mode
    // Cloud => retain=false (spart Rule-Actions / retained churn)
    const bool retainTelemetry = !mqttCloudMode;

    // --- STATES (/message) ---
    bwc->getJSONStates(json);
    if (mqttClient->publish((String(mqttBaseTopic) + F("/message")).c_str(), json.c_str(), retainTelemetry))
    {
        BWC_LOG_P(PSTR("MQTT > message published\n"),0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > message not published"),0);
    }

    // --- TIMES (/times) ---
    // ✅ In Cloud-Mode optional komplett weglassen (dein Wunsch)
    // HomeAssistant/Custom bleibt unverändert.
    if (!mqttCloudMode)
    {
        json.clear();
        bwc->getJSONTimes(json);
        if (mqttClient->publish((String(mqttBaseTopic) + F("/times")).c_str(), json.c_str(), retainTelemetry))
        {
            BWC_LOG_P(PSTR("MQTT > times published"),0);
        }
        else
        {
            BWC_LOG_P(PSTR("MQTT > times not published"),0);
        }
    }

    // --- OTHER (/other) ---
    // ✅ brauchst du in Cloud (sagst du), daher immer senden
    json.clear();
    getOtherInfo(json);
    if (mqttClient->publish((String(mqttBaseTopic) + F("/other")).c_str(), json.c_str(), retainTelemetry))
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
    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);

    const bool retainCfg = !mqttCloudMode; // Cloud => false, Custom/Home => true

    mqttClient->publish((String(mqttBaseTopic) + F("/get_config")).c_str(), json.c_str(), retainCfg);
    mqttClient->loop();
}


/**
 * Start a Wi-Fi access point, and try to connect to some given access points.
 * Then wait for either an AP or STA connection
 */
void startWiFi()
{
    BWC_LOG_P(PSTR("startWiFi() @ millis: %d\n"), millis());
    //WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.hostname(netHostname);
    loadWifi();


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
            // disable specific WiFi config
            wifi_info.enableAp = false;
            wifi_info.enableStaticIp4 = false;
            // fallback to WiFi config portal
            startWiFiConfigPortal();
        }
    }
}

/**
 * start WiFiManager configuration portal
 */
void startWiFiConfigPortal()
{
    Serial.println(F("WiFi > Using WiFiManager Config Portal"));
    ESP_WiFiManager wm;
    wm.autoConnect(wmApName, wmApPassword);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
    }
}

void checkNTP_ISR()
{
    checkNTP_flag = true;
}

void checkNTP()
{
    time_t now = time(nullptr);
    static uint8_t ntpTryNumber = 0;
    if(now < 8 * 3600 * 2)
    {
        if (++ntpTryNumber == 10) {
            ntpTryNumber = 0; //reset until next check
            ntpCheck_ticker.detach();
        }
        return;
    }
    ntpCheck_ticker.detach(); //time is set, don't check again
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    time_t boot_timestamp = getBootTime();
    tm * boot_time_tm = gmtime(&boot_timestamp);
    char boot_time_str[64];
    strftime(boot_time_str, 64, "%F %T", boot_time_tm);
    bwc->reboot_time_str = String(boot_time_str);
    bwc->reboot_time_t = boot_timestamp;
    bwc->saveRebootInfo();
}

/**
 * start NTP sync
 */
void startNTP()
{
    Serial.println(F("start NTP"));
    configTime(0,0,wifi_info.ip4NTP_str, F("pool.ntp.org"), F("time.nist.gov"));
    ntpCheck_ticker.attach(0.5, checkNTP_ISR);
}

void startOTA()
{
    ArduinoOTA.setHostname(OTAName);
    ArduinoOTA.setPassword(OTAPassword);

    ArduinoOTA.onStart([]() {
        // Serial.println(F("OTA > Start"));
        stopall();
    });
    ArduinoOTA.onEnd([]() {
        // Serial.println(F("OTA > End"));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // Serial.printf("OTA > Progress: %u%%\r\n", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        // Serial.printf("OTA > Error[%u]: ", error);
        // if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
        // else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
        // else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
        // else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
        // else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
    });
    ArduinoOTA.begin();
    // Serial.println(F("OTA > ready"));
}

void stopall()
{
    Serial.printf_P(PSTR("Free mem before stop: %d\n"), ESP.getFreeHeap());
    bwc->stop();

    Serial.println(F("detaching"));
    updateMqttTimer.detach();
    periodicTimer.detach();
    updateWSTimer.detach();
    if (ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    if (checkWifi_ticker.active()) checkWifi_ticker.detach();

    // --- sensors ---
    if (tempSensors) { delete tempSensors; tempSensors = nullptr; }
    if (oneWire)     { delete oneWire;     oneWire     = nullptr; }

    // --- MQTT/TLS ---
    Serial.println(F("stopping mqtt"));

    // Crash-Fix: MQTT/TLS sind statisch – nichts löschen, nur sauber trennen
    if (mqttClient) {
        if (mqttClient->connected()) mqttClient->disconnect();
    }

#if defined(ESP8266)
    aWifiClient = tlsClient; // zeigt auf statischen TLS Client
    if (tlsClient) tlsClient->stop();
    // tlsCa bleibt statisch (keine Aktion nötig)
    tlsClient  = &tlsClientStatic;
    tlsCa      = &tlsCaStatic;
    aWifiClient = tlsClient;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);
#else
    aWifiClient = &wifiClientStatic;
    mqttClient  = &mqttClientStatic;
    mqttClient->setClient(*aWifiClient);
#endif


    // --- Presence TLS (Cloud Functions) ---
    presenceClient.stop(); // Verbindung schließen
    if (presenceCa)
    {
        delete presenceCa;
        presenceCa = nullptr;
    }
    presenceTlsReady = false;



    // --- server/ws/fs (auch mit Guards, damit's nicht crasht) ---
    Serial.println(F("stopping server"));
    if (server) { server->stop(); delete server; server = nullptr; }

    Serial.println(F("stopping ws"));
    if (webSocket) { webSocket->close(); delete webSocket; webSocket = nullptr; }

    Serial.println(F("stopping FS"));
    LittleFS.end();

    Serial.println(F("end stopall"));
    Serial.printf_P(PSTR("Free mem after stop: %d\n"), ESP.getFreeHeap());
}


/*pause: action=true cont: action=false*/
void pause_all(bool action)
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
        updateWSTimer.attach(2.0, []{ sendWSFlag = true; });
        //bootlogTimer.attach(5, []{ if(DateTime.isTimeValid()) {bwc->saveRebootInfo(); bootlogTimer.detach();} });
    }
    bwc->pause_all(action);
}

void startWebSocket()
{
    HeapSelectIram ephemeral;
    Serial.printf_P(PSTR("WS IRamheap %d\n"), ESP.getFreeHeap());
    if(webSocket != nullptr)
    {
        webSocket->disconnect();
        webSocket->close();
        delete webSocket;
        webSocket = nullptr;
    }
    webSocket = new WebSocketsServer(81);
    webSocket->begin();
    webSocket->enableHeartbeat(3000, 3000, 1);
    webSocket->onEvent(webSocketEvent);
    // Serial.println(F("WebSocket > server started"));
}

/**
 * handle web socket events
 */
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t len)
{
    // When a WebSocket message is received
    switch (type)
    {
        // if the websocket is disconnected
        case WStype_DISCONNECTED:
        // Serial.printf("WebSocket > [%u] Disconnected!\r\n", num);
        break;

        // if a new websocket connection is established
        case WStype_CONNECTED:
        {
            // IPAddress ip = webSocket->remoteIP(num);
            // Serial.printf("WebSocket > [%u] Connected from %d.%d.%d.%d url: %s\r\n", num, ip[0], ip[1], ip[2], ip[3], payload);
            sendWS();
        }
        break;

        // if new text data is received
        case WStype_TEXT:
        {
            // Serial.printf("WebSocket > [%u] get Text: %s\r\n", num, payload);
            // DynamicJsonDocument doc(256);
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (error)
            {
            Serial.println(F("WebSocket > JSON command failed"));
            return;
            }

            // Copy values from the JsonDocument to the Config
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
            if (bwc)
            {
                // Manual HEAT/PUMP control from the main page overrides Smart Schedule.
                bwc->handleSmartScheduleWebOverride(command);
                bwc->add_command(item);
            }
        }
        break;

        default:
        break;
    }
}

/**
 * start a HTTP server with a file read and upload handler
 */
void handleDiag();

void handleGetSmartSchedule();
void handleSetSmartSchedule();
void handleUpdateSmartSchedule();
void handleCancelSmartSchedule();

void startHttpServer()
{
    if(server != nullptr)
    {
        server->stop();
        server->close();
        delete server;
        server = nullptr;
    }

    {
        // HeapSelectIram ephemeral;
        server = new ESP8266WebServer(80);
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
        server->on(F("/metrics"), handlePrometheusMetrics);  //prometheus metrics
        server->on(F("/info/"), handleESPInfo);
        server->on(F("/sethardware/"), handleSetHardware);
        server->on(F("/gethardware/"), handleGetHardware);
        server->on(F("/debug-on/"), [](){bwc->BWC_DEBUG = true; server->send(200, F("text/plain"), "ok");});
        server->on(F("/debug-off/"), [](){bwc->BWC_DEBUG = false; server->send(200, F("text/plain"), "ok");});
        server->on(F("/cmdq_file/"), handle_cmdq_file);

        // if someone requests any other file or page, go to function 'handleNotFound'
        // and check if the file exists
        server->onNotFound(handleNotFound);
        // start the HTTP server
        server->begin();
    }
    
    // Serial.println(F("HTTP > server started"));
}

void handleGetHardware()
{
    if (!checkHttpPost(server->method())) return;
    File file = LittleFS.open(F("hwcfg.json"), "r");
    if (!file)
    {
        // Serial.println(F("Failed to open hwcfg.json"));
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
    // Serial.printf("Set hw message; %s\n", message.c_str());
    File file = LittleFS.open(F("hwcfg.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save hwcfg.json"));
        return;
    }
    file.print(message);
    file.close();
    server->send(200, F("text/plain"), "ok");
    // Serial.println("sethardware done");
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
    unsigned long t = millis(); //start timestamp

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

    /* send statistics to client */
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

    /* First test CIO out/ DSP in ports */
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

    /* Test the other way around */

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
        /*CIO pins HIGH*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], HIGH);
        }
        delay(5000);
        /*CIO pins LOW*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+3], INPUT);
            pinMode(bwc->pins[pin+0], OUTPUT);
            digitalWrite(bwc->pins[pin], LOW);
        }
        delay(5000);
        /*DSP pins HIGH*/
        for(int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin+0], INPUT);
            pinMode(bwc->pins[pin+3], OUTPUT);
            digitalWrite(bwc->pins[pin+3], HIGH);
        }
        delay(5000);
        /*DSP pins LOW*/
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

void handleNotFound()
{
    // check if the file exists in the flash memory (LittleFS), if so, send it
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

/**
 * send the right file to the client (if it exists)
 */
bool handleFileRead(String path)
{
    pause_all(true);
    // Serial.println("HTTP > request: " + path);
    // If a folder is requested, send the index file
    if (path.endsWith("/"))
    {
        path += F("index.html");
    }
    // deny reading credentials
    if (path.equalsIgnoreCase("/mqtt.json") || path.equalsIgnoreCase("/wifi.json"))
    {
        server->send(403, F("text/plain"), F("Permission denied."));
        // Serial.println(F("HTTP > file reading denied (credentials)."));
        pause_all(false);
        return false;
    }

    String contentType = getContentType(path);                  // Get the MIME type
    String pathWithGz = path + ".gz";
    if (LittleFS.exists(pathWithGz) || LittleFS.exists(path)) { // If the file exists, either as a compressed archive, or normal
        if (LittleFS.exists(pathWithGz))                        // If there's a compressed version available
            path += ".gz";                                      // Use the compressed version
        File file = LittleFS.open(path, "r");                   // Open the file
        size_t fsize = file.size();
        size_t sent = server->streamFile(file, contentType);    // Send it to the client
        
        file.close();                                           // Close the file again
        Serial.println(F("File size: ") + String(fsize));
        Serial.println(F("HTTP > file sent: ") + path + F(" (") + sent + F(" bytes)"));
        if(fsize != sent){
            Serial.println(F("********* File not completed *******"));
        }
        pause_all(false);
        return true;
    }
    // Serial.println("HTTP > file not found: " + path);   // If the file doesn't exist, return false
    pause_all(false);
    return false;
}

/**
 * checks the method to be a POST
 */
bool checkHttpPost(HTTPMethod method)
{
    if (method != HTTP_POST)
    {
        server->send(405, F("text/plain"), F("Method not allowed."));
        return false;
    }
    return true;
}

/**
 * response for /getconfig/
 * web server prints a json document
 */

void handleDiag()
{
    if(server == nullptr) return;

    String out;
    out.reserve(1024);

    out += F("<html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1'>"
             "<title>ESP8266 /diag</title></head><body>"
             "<h2>ESP8266 Diagnostics</h2><pre>");

    out += htmlEscape(g_boot_diag);

    out += F("\n\n--- Live ---\nUptime(ms): ");
    out += String(millis());
    out += F("\nHeap: ");
    out += String(ESP.getFreeHeap());
    out += F("\nmaxBlock: ");
    out += String(ESP.getMaxFreeBlockSize());
    out += F("\nfrag: ");
    out += String(ESP.getHeapFragmentation());
    out += F("%\nWiFi: ");
    out += (WiFi.status() == WL_CONNECTED) ? F("connected") : F("not connected");
    out += F("\nIP: ");
    out += WiFi.localIP().toString();

    out += F("</pre></body></html>");

    server->send(200, F("text/html; charset=utf-8"), out);
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
    bwc->getJSONSettings(json);
    server->send(200, F("text/plain"), json);
}

/**
 * response for /setconfig/
 * web server writes a json document
 */
void handleSetConfig()
{
    if (!checkHttpPost(server->method())) return;

    String message = server->arg(0);
    bwc->setJSONSettings(message);

    server->send(200, F("text/plain"), "");
    send_mqtt_cfg_needed = true;
}

/**
 * response for /getcommands/
 * web server prints a json document
 */
void handleGetCommandQueue()
{
    if (!checkHttpPost(server->method())) return;

    String json = bwc->getJSONCommandQueue();
    server->send(200, F("application/json"), json);
}

/**
 * response for /addcommand/
 * add a command to the queue
 */
void handleAddCommand()
{
    // if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
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
    bwc->add_command(item);

    server->send(200, F("text/plain"), F("ok"));
}

/**
 * response for /editcommand/
 * replace a command in the queue with new command
 */
void handleEditCommand()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
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
    bwc->edit_command(index, item);

    server->send(200, F("text/plain"), "");
}

/**
 * response for /delcommand/
 * replace a command in the queue with new command
 */
void handleDelCommand()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    uint8_t index = doc[F("IDX")];
    bwc->del_command(index);

    server->send(200, F("text/plain"), "");
}

void handle_cmdq_file()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
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
    char ibuffer[64];  //declare a buffer
    
    File f_source = LittleFS.open(source, "r");    //open source file to read
    if (!f_source)
    {
        return;
    }

    File f_dest = LittleFS.open(dest, "w");    //open destination file to write
    if (!f_dest)
    {
        return;
    }
    
    while (f_source.available() > 0)
    {
        byte i = f_source.readBytes(ibuffer, 64); // i = number of bytes placed in buffer from file f_source
        f_dest.write(ibuffer, i);               // write i bytes from buffer to file f_dest
    }
    
    f_dest.close(); // done, close the destination file
    f_source.close(); // done, close the source file
}

/**
 * load "Web Config" json configuration from "webconfig.json"
 */
void loadWebConfig()
{
    // DynamicJsonDocument doc(1024);
    StaticJsonDocument<256> doc;

    File file = LittleFS.open(F("/webconfig.json"), "r");
    if (file)
    {
        DeserializationError error = deserializeJson(doc, file);
        if (error)
        {
        // Serial.println(F("Failed to deserialize webconfig.json"));
        file.close();
        return;
        }
    }
    else
    {
        // Serial.println(F("Failed to read webconfig.json. Using defaults."));
    }

    showSectionTemperature = (doc.containsKey(F("SST")) ? doc[F("SST")] : true);
    showSectionDisplay = (doc.containsKey(F("SSD")) ? doc[F("SSD")] : true);
    showSectionControl = (doc.containsKey(F("SSC")) ? doc[F("SSC")] : true);
    showSectionButtons = (doc.containsKey(F("SSB")) ? doc[F("SSB")] : true);
    showSectionTimer = (doc.containsKey(F("SSTIM")) ? doc[F("SSTIM")] : true);
    showSectionTotals = (doc.containsKey(F("SSTOT")) ? doc[F("SSTOT")] : true);
    useControlSelector = (doc.containsKey(F("UCS")) ? doc[F("UCS")] : false);
}

/**
 * save "Web Config" json configuration to "webconfig.json"
 */
void saveWebConfig()
{
    File file = LittleFS.open(F("/webconfig.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save webconfig.json"));
        return;
    }

    // DynamicJsonDocument doc(256);
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
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
}

/**
 * response for /getwebconfig/
 * web server prints a json document
 */
void handleGetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
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

/**
 * response for /setwebconfig/
 * web server writes a json document
 */
void handleSetWebConfig()
{
    if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
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

/**
 * load WiFi json configuration from "wifi.json"
 */
void loadWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "r");
    if (!file)
    {
        // Serial.println(F("Failed to read wifi.json. Using defaults."));
        return;
    }

    DynamicJsonDocument doc(1024);

    DeserializationError error = deserializeJson(doc, file);
    if (error)
    {
        // Serial.println(F("Failed to deserialize wifi.json"));
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

    // ip4Address[0] = doc[F("ip4Address")][0];
    // ip4Address[1] = doc[F("ip4Address")][1];
    // ip4Address[2] = doc[F("ip4Address")][2];
    // ip4Address[3] = doc[F("ip4Address")][3];
    // ip4Gateway[0] = doc[F("ip4Gateway")][0];
    // ip4Gateway[1] = doc[F("ip4Gateway")][1];
    // ip4Gateway[2] = doc[F("ip4Gateway")][2];
    // ip4Gateway[3] = doc[F("ip4Gateway")][3];
    // ip4Subnet[0] = doc[F("ip4Subnet")][0];
    // ip4Subnet[1] = doc[F("ip4Subnet")][1];
    // ip4Subnet[2] = doc[F("ip4Subnet")][2];
    // ip4Subnet[3] = doc[F("ip4Subnet")][3];
    // ip4DnsPrimary[0] = doc[F("ip4DnsPrimary")][0];
    // ip4DnsPrimary[1] = doc[F("ip4DnsPrimary")][1];
    // ip4DnsPrimary[2] = doc[F("ip4DnsPrimary")][2];
    // ip4DnsPrimary[3] = doc[F("ip4DnsPrimary")][3];
    // ip4DnsSecondary[0] = doc[F("ip4DnsSecondary")][0];
    // ip4DnsSecondary[1] = doc[F("ip4DnsSecondary")][1];
    // ip4DnsSecondary[2] = doc[F("ip4DnsSecondary")][2];
    // ip4DnsSecondary[3] = doc[F("ip4DnsSecondary")][3];
}

/**
 * save WiFi json configuration to "wifi.json"
 */
void saveWifi()
{
    File file = LittleFS.open(F("/wifi.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save wifi.json"));
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
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
}

/**
 * response for /getwifi/
 * web server prints a json document
 */
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

/**
 * response for /setwifi/
 * web server writes a json document
 */
void handleSetWifi()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
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

/*
 * response for /resetwifi/
 * do this before giving away the device (be aware of other credentials e.g. MQTT)
 * a complete flash erase should do the job but remember to upload the filesystem as well.
 */
void handleResetWifi()
{
    server->send(200, F("text/html"), F("WiFi connection reset (erase) ..."));
    // Serial.println(F("WiFi connection reset (erase) ..."));
    resetWiFi();

    server->send(200, F("text/html"), F("WiFi connection reset (erase) ... done."));
    // Serial.println(F("WiFi connection reset (erase) ... done."));
    // Serial.println(F("ESP reset ..."));
    #if defined(ESP8266)
    requestRestart(__FUNCTION__);
    #else
    requestRestart(__FUNCTION__);
    #endif
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
    updateMqttTimer.detach();
    updateWSTimer.detach();
    if(ntpCheck_ticker.active()) ntpCheck_ticker.detach();
    bwc->saveSettings();
    bwc->stop();
    delay(1000);
#if defined(ESP8266)
    ESP.eraseConfig();
#endif
    delay(1000);
    ESP_WiFiManager wm;
    wm.resetSettings();
    //WiFi.disconnect();
    delay(1000);
}

/**
 * load MQTT json configuration from "mqtt.json"
 */
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
        // Serial.println(F("Failed to deserialize mqtt.json."));
        file.close();
        return;
    }

    useMqtt = doc[F("enableMqtt")];
    // enableMqtt = useMqtt; //will be set with start complete timer
    mqttIpAddress[0] = doc[F("mqttIpAddress")][0];
    mqttIpAddress[1] = doc[F("mqttIpAddress")][1];
    mqttIpAddress[2] = doc[F("mqttIpAddress")][2];
    mqttIpAddress[3] = doc[F("mqttIpAddress")][3];
    mqttPort = doc[F("mqttPort")];
    mqttUsername = doc[F("mqttUsername")].as<String>();
    mqttPassword = doc[F("mqttPassword")].as<String>();
    mqttClientId = doc[F("mqttClientId")].as<String>();
    mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];
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

    // Wenn sich der Code wirklich geändert hat: 10s MQTT erlauben (Cloud-Mode)
    if (mqttCloudMode && newCode.length() >= 4 && newCode != oldCode)
    {
        cloud_pair_bootstrap_until_ms = millis() + 10000UL; // 10 Sekunden
        Serial.println(F("PAIRING: bootstrap MQTT enabled for 10s"));
        cloud_next_mqtt_try_ms = 0;   // sofortiger Connect-Versuch erlaubt

// sofort versuchen (nicht warten bis loop tickt)
if (WiFi.status() == WL_CONNECTED && enableMqtt && mqttClient && !mqttClient->connected()) {
  Serial.println(F("PAIRING: bootstrap -> mqttConnect() now"));
  mqttConnect();
}

    }
}



}



/**
 * save MQTT json configuration to "mqtt.json"
 */
void saveMqtt()
{
    File file = LittleFS.open("mqtt.json", "w");
    if (!file)
    {
        // Serial.println(F("Failed to save mqtt.json"));
        return;
    }

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = useMqtt;
    doc[F("mqttIpAddress")][0] = mqttIpAddress[0];
    doc[F("mqttIpAddress")][1] = mqttIpAddress[1];
    doc[F("mqttIpAddress")][2] = mqttIpAddress[2];
    doc[F("mqttIpAddress")][3] = mqttIpAddress[3];
    doc[F("mqttPort")] = mqttPort;
    doc[F("mqttUsername")] = mqttUsername;
    doc[F("mqttPassword")] = mqttPassword;
    doc[F("mqttClientId")] = mqttClientId;
    doc[F("mqttBaseTopic")] = mqttBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqttTelemetryInterval;
    
    doc[F("mqttMode")] = mqttCloudMode ? "cloud" : "custom";
    doc[F("mqttPairingCode")] = mqttPairingCode;



    if (serializeJson(doc, file) == 0)
    {
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
}

/**
 * response for /getmqtt/
 * web server prints a json document
 */
void handleGetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = useMqtt;
    doc[F("mqttIpAddress")][0] = mqttIpAddress[0];
    doc[F("mqttIpAddress")][1] = mqttIpAddress[1];
    doc[F("mqttIpAddress")][2] = mqttIpAddress[2];
    doc[F("mqttIpAddress")][3] = mqttIpAddress[3];
    doc[F("mqttPort")] = mqttPort;
    doc[F("mqttUsername")] = mqttUsername;
    doc[F("mqttPassword")] = "<enter password>";
    if (!hidePasswords)
    {
        doc[F("mqttPassword")] = mqttPassword;
    }
    doc[F("mqttClientId")] = mqttClientId;
    doc[F("mqttBaseTopic")] = mqttBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqttTelemetryInterval;

    doc[F("mqttMode")] = mqttCloudMode ? "cloud" : "custom";
    doc[F("deviceMac")] = WiFi.macAddress();
    doc[F("deviceId")]  = getMacClean();
    doc[F("mqttPairingCode")] = mqttPairingCode;



    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("text/plain"), json);
}

/**
 * response for /setmqtt/
 * web server writes a json document
 */
void handleSetMqtt()
{
    if (!checkHttpPost(server->method())) return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    useMqtt = doc[F("enableMqtt")];
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
        cloud_pair_bootstrap_until_ms = millis() + 10000UL; // 10 Sekunden
        Serial.println(F("PAIRING: bootstrap MQTT enabled for 10s"));
        cloud_next_mqtt_try_ms = 0;   // sofortiger Connect-Versuch erlaubt

// sofort versuchen (nicht warten bis loop tickt)
if (WiFi.status() == WL_CONNECTED && enableMqtt && mqttClient && !mqttClient->connected()) {
  Serial.println(F("PAIRING: bootstrap -> mqttConnect() now"));
  mqttConnect();
}

    }
}


    // Nur im Custom Mode überschreiben wir Host/User/Pass/Topic
    if (!mqttCloudMode)
    {
        mqttIpAddress[0] = doc[F("mqttIpAddress")][0];
        mqttIpAddress[1] = doc[F("mqttIpAddress")][1];
        mqttIpAddress[2] = doc[F("mqttIpAddress")][2];
        mqttIpAddress[3] = doc[F("mqttIpAddress")][3];
        mqttPort = doc[F("mqttPort")];
        mqttUsername = doc[F("mqttUsername")].as<String>();
        mqttPassword = doc[F("mqttPassword")].as<String>();
        mqttClientId = doc[F("mqttClientId")].as<String>();
        mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    }

    // Telemetry Interval darf in beiden Modi gesetzt werden
    mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];



    server->send(200, F("text/plain"), "");
        


    saveMqtt();
    startMqtt();

    // Wenn MQTT schon verbunden ist, sofort aktualisieren.
    // Falls startMqtt() neu verbindet, passiert es ohnehin nochmal bei mqttConnect().
    if (mqttClient && mqttClient->connected()) {
        publishPairingHash();
}

}

/**
 * response for /dir/
 * web server prints a list of files
 */
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
        // Serial.println(root.fileName());
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

/**
 * response for /upload.html
 * upload a new file to the LittleFS
 */
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

        // The file server always prefers a compressed version of a file
        if (!path.endsWith(".gz"))
        {
            // So if an uploaded file is not compressed, the existing compressed
            String pathWithGz = path + ".gz";
            // version of that file must be deleted (if it exists)
            if (LittleFS.exists(pathWithGz))
            {
                LittleFS.remove(pathWithGz);
            }
        }

        Serial.print(F("handleFileUpload Name: "));
        Serial.println(path);

        // Open the file for writing in LittleFS (create if it doesn't exist)
        fsUploadFile = LittleFS.open(path, "w");
        path = String();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (fsUploadFile)
        {
            // Write the received bytes to the file
            fsUploadFile.write(upload.buf, upload.currentSize);
            // Serial.print("file write ");
            // Serial.println(path);
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

/**
 * response for /remove.html
 * delete a file from the LittleFS
 */
void handleFileRemove()
{
    String path;
    path = server->arg(F("FileToRemove"));
    if (!path.startsWith("/"))
    {
        path = "/" + path;
    }

    // Serial.print(F("handleFileRemove Name: "));
    // Serial.println(path);

    if (LittleFS.exists(path) && LittleFS.remove(path))
    {
        // Serial.print(F("handleFileRemove success: "));
        // Serial.println(path);
        if(server->method() == HTTP_GET)
            server->sendHeader(F("Location"), F("/dir/"));
        else
            server->sendHeader(F("Location"), F("/success.html"));
        server->send(303);
    }
    else
    {
        // Serial.print(F("handleFileRemove error: "));
        // Serial.println(path);
        server->send(500, F("text/plain"), F("500: couldn't delete file"));
    }
}

/**
 * response for /restart/
 */
void handleRestart()
{
    server->send(200, F("text/html"), F("ESP restart ..."));

    server->sendHeader(F("Location"), "/");
    server->send(303);

    delay(1000);
    stopall();
    delay(1000);
    Serial.println(F("ESP restart ..."));
    requestRestart(__FUNCTION__);
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

/**
 * MQTT setup and connect
 * @author 877dev
 */
void startMqtt()
{
    Serial.printf_P(PSTR("DRAM heap before MQTT: %u\n"), ESP.getFreeHeap());
    {
        HeapSelectIram e;
        Serial.printf_P(PSTR("IRAM heap before MQTT: %u\n"), ESP.getFreeHeap());
    }

    Serial.println(F("startmqtt"));

    // load mqtt credential file if it exists, and update default strings
    loadMqtt();

#if defined(ESP8266)
    // Crash-Fix: statische TLS-Objekte (kein new/delete)
    tlsClient = &tlsClientStatic;
    tlsCa     = &tlsCaStatic;

    tlsClient->setTrustAnchors(tlsCa);

    // ✅ RAM sparen (wichtig!)
    tlsClient->setBufferSizes(512, 512);

    // ✅ Timeout nicht zu hoch, spart RAM/Blockaden
    tlsClient->setTimeout(15);

    aWifiClient = tlsClient;
#else
    // Crash-Fix: statischer Plain-WiFi Client
    aWifiClient = &wifiClientStatic;
#endif


    mqttClient = &mqttClientStatic; mqttClient->setClient(*aWifiClient);

// PubSub buffer: Cloud kleiner, Custom ggf. größer
mqttClient->setBufferSize(mqttCloudMode ? 1024 : 1536);


    // disconnect in case we are already connected
    mqttClient->disconnect();

// Device-ID (immer MAC-clean)
String devId = getMacClean();

// ClientId (kurz & stabil)
mqttClientId = devId;

// ------------------------------
// Dual-Mode MQTT (Cloud vs Custom)
// ------------------------------
if (mqttCloudMode)
{
    // Cloud: immer EMQX + festes SAR BaseTopic
    mqttBaseTopic = String(SAR_TOPIC_PREFIX) + devId;

    mqttUsername = SAR_EMQX_USER;
    mqttPassword = SAR_EMQX_PASS;
    mqttPort     = SAR_EMQX_PORT;
    mqttClient->setServer(SAR_EMQX_HOST, mqttPort);
}
else
{
    // Custom: Broker/Topic aus mqtt.json verwenden (NICHT überschreiben!)
    // mqttBaseTopic/mqttUsername/mqttPassword/mqttPort wurden in loadMqtt() gesetzt

    // mqttIpAddress[] kommt aus mqtt.json
    mqttClient->setServer(mqttIpAddress, mqttPort);

    // Optional: wenn mqttClientId im Custom gesetzt ist, dann nimm den
    if (mqttClientId.length() == 0) mqttClientId = devId;
}



    mqttClient->setKeepAlive(60);
    mqttClient->setSocketTimeout(30);
    mqttClient->setCallback(mqttCallback);

    Serial.printf_P(PSTR("DRAM heap after MQTT init: %u\n"), ESP.getFreeHeap());
    {
        HeapSelectIram e;
        Serial.printf_P(PSTR("IRAM heap after MQTT init: %u\n"), ESP.getFreeHeap());
    }

    // Nicht sofort verbinden – im Cloud-Mode steuert Presence das
if (!mqttCloudMode) {
  mqttConnect();  // HomeAssistant/Custom bleibt wie gewohnt
}

}


/**
 * MQTT callback function
 * @author 877dev
 */
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    // Payload sicher in String kopieren (MQTT payload ist NICHT null-terminiert)
    String message;
    message.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++)
        message += (char)payload[i];

    String t = String(topic);

    // ---------- /command ----------
    if (t.equals(String(mqttBaseTopic) + F("/command")))
    {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, message);
        if (error) return;

        Commands command = doc[F("CMD")];
        int64_t value    = doc[F("VALUE")];
        int64_t xtime    = doc[F("XTIME")];
        int64_t interval = doc[F("INTERVAL")];
        String txt       = doc[F("TXT")] | "";

        command_que_item item;
        item.cmd      = command;
        item.val      = value;
        item.xtime    = xtime;
        item.interval = interval;
        item.text     = txt;

        bwc->add_command(item);
        return;
    }

    // ---------- /command_batch ----------
    if (t.equals(String(mqttBaseTopic) + F("/command_batch")))
    {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, message);
        if (error) return;

        JsonArray commandArray = doc.as<JsonArray>();

        for (JsonVariant commandItem : commandArray)
        {
            Commands command = commandItem[F("CMD")];
            int64_t value    = commandItem[F("VALUE")];
            int64_t xtime    = commandItem[F("XTIME")];
            int64_t interval = commandItem[F("INTERVAL")];

            // ✅ FIX: TXT aus dem jeweiligen commandItem lesen, nicht aus doc
            String txt = commandItem[F("TXT")] | "";

            command_que_item item;
            item.cmd      = command;
            item.val      = value;
            item.xtime    = xtime;
            item.interval = interval;
            item.text     = txt;

            bwc->add_command(item);
        }
        return;
    }

    // ---------- /set_config ----------
    if (t.equals(String(mqttBaseTopic) + F("/set_config")))
    {
        bwc->setJSONSettings(message);
        send_mqtt_cfg_needed = true;
        return;
    }
}


/**
 * Connect to MQTT broker, publish Status/MAC/count, and subscribe to keypad topic.
 */
void mqttConnect()
{
    // In Cloud-Mode kann MQTT auch dann verbinden, wenn enableMqtt=false,
    // solange Presence (oder Bootstrap) es erlaubt. enableMqtt ist in deinem
    // Projekt historisch der "lokale MQTT-Schalter" (HA/Custom).
    if (!enableMqtt)
    {
        if (!(mqttCloudMode && (presence_allowed || cloudBootstrapActive())))
        {
            return;
        }
    }
    Serial.println(F("mqttconn"));
#if defined(ESP8266)
    mqtt_last_attempt_ms = millis();
#endif
    // TLS braucht gültige Uhrzeit
waitForValidTime(8000);


#ifdef ESP8266
  if (tlsClient) tlsClient->setX509Time(time(nullptr));
#endif

    // Serial.print(F("MQTT > Connecting ... "));
    // We'll connect with a Retained Last Will that updates the 'Status' topic with "Dead" when the device goes offline...
    if (mqttClient->connect(
        mqttClientId.c_str(), // client_id : the client ID to use when connecting to the server->
        mqttUsername.c_str(), // username : the username to use. If NULL, no username or password is used (const char[])
        mqttPassword.c_str(), // password : the password to use. If NULL, no password is used (const char[])setupHA
        (String(mqttBaseTopic) + F("/Status")).c_str(), // willTopic : the topic to be used by the will message (const char[])
        0, // willQoS : the quality of service to be used by the will message (int : 0,1 or 2)
        1, // willRetain : whether the will should be published with the retain flag (int : 0 or 1)
        "Dead")) // willMessage : the payload of the will message (const char[])
        
    {
        // Serial.println(F("success!"));
        mqtt_connect_count++;
#if defined(ESP8266)
        mqtt_last_connected_ms = millis();
        mqtt_fail_streak = 0;
#endif

if (mqttCloudMode)
{
    // Polling wird erst in der Cloud-Loop nach stabiler Verbindung pausiert
    presence_polling_paused = false;
}



// update MQTT every X seconds. (will also be updated on state changes)
// ✅ Im Cloud Duty-Cycle Mode KEIN Ticker, sonst triggert es unnötig außerhalb des Fensters
if (!mqttCloudMode)
{
    updateMqttTimer.attach(mqttTelemetryInterval, []{ sendMQTTFlag = true; });
}



        

        // These all have the Retained flag set to true, so that the value is stored on the server and can be retrieved at any point
        // Check the 'Status' topic to see that the device is still online before relying on the data from these retained topics
const bool retainIdentity = !mqttCloudMode;   // Cloud: false, Custom: true
const bool retainStatus   = true;            // Status retained ist sinnvoll (LWT + Online-Check)

mqttClient->publish((String(mqttBaseTopic) + F("/Status")).c_str(), "Alive", retainStatus);
mqttClient->publish((String(mqttBaseTopic) + F("/MAC_Address")).c_str(), WiFi.macAddress().c_str(), retainIdentity);
mqttClient->publish((String(mqttBaseTopic) + F("/MQTT_Connect_Count")).c_str(), String(mqtt_connect_count).c_str(), retainIdentity);

        mqttClient->loop();

        publishPairingHash();   // <-- HINZUFÜGEN
        mqttClient->loop();


        // Watch the 'command' topic for incoming MQTT messages
        mqttClient->subscribe((String(mqttBaseTopic) + F("/command")).c_str());
        mqttClient->subscribe((String(mqttBaseTopic) + F("/command_batch")).c_str());
        mqttClient->subscribe((String(mqttBaseTopic) + F("/set_config")).c_str());
        mqttClient->loop();

#ifdef ESP8266
    // Diese HomeAssistant-spezifischen Publishes/Discovery nur im Custom/Home Mode
    if (!mqttCloudMode)
    {
        mqttClient->publish((String(mqttBaseTopic) + F("/reboot_time")).c_str(), (bwc->reboot_time_str + 'Z').c_str(), true);
        mqttClient->publish((String(mqttBaseTopic) + F("/reboot_reason")).c_str(), ESP.getResetReason().c_str(), true);

        String buttonname;
        buttonname.reserve(32);
        bwc->getButtonName(buttonname);

        const bool retainButton = true; // Home/Custom retained wie gewohnt
        mqttClient->publish((String(mqttBaseTopic) + F("/button")).c_str(), buttonname.c_str(), retainButton);

        mqttClient->loop();
        sendMQTT();

        Serial.println(F("MQTT Sending HA discovery"));
        setupHA();

        mqttClient->loop();
        send_mqtt_cfg_needed = true;
        Serial.println(F("done"));
    }
#endif

    }
    else
    {
        Serial.print(F("MQTT connect FAILED, state="));
        Serial.println(mqttClient->state());
#if defined(ESP8266)
        if (mqtt_fail_streak < 250) mqtt_fail_streak++;
#endif

        // optional: damit du es sofort siehst, woran es grob liegt:
        // -4 timeout, -3 lost, -2 failed, -1 disconnected, 1 bad protocol,
        // 2 bad client id, 3 unavailable, 4 bad creds, 5 unauthorized
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
            //float temperatureF = tempSensors.getTempFByIndex(0);
            //Serial.print(temperatureC);
            //Serial.println("ºC");
            //Serial.print(temperatureF);
            //Serial.println("ºF");

            // Ignore bad reads
            if(temperatureC >= -20.0)
            {
                bwc->setAmbientTemperature(temperatureC, true);
            }
    }
}

#include "ha.txt"
#include "prometheus.txt"
