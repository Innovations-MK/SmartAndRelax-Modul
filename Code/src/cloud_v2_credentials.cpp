#include "cloud_v2_credentials.h"

#if defined(ESP8266)
#include <EEPROM.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

namespace {
constexpr uint32_t CLOUD_V2_MAGIC = 0x32524153UL; 
constexpr uint8_t CLOUD_V2_VERSION = 1;
constexpr size_t CLOUD_V2_SECRET_LEN = 64;        
constexpr size_t CLOUD_V2_EEPROM_SIZE = 128;

struct __attribute__((packed)) CloudV2Record {
    uint32_t magic;
    uint8_t version;
    uint8_t secretLength;
    char secret[CLOUD_V2_SECRET_LEN + 1];
    uint32_t crc32;
};

static char g_secret[CLOUD_V2_SECRET_LEN + 1] = {0};
static bool g_provisioned = false;

uint32_t crc32Calc(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return ~crc;
}

bool secretValid(const String& in) {
    if (in.length() != CLOUD_V2_SECRET_LEN) return false;
    for (size_t i = 0; i < CLOUD_V2_SECRET_LEN; ++i)
        if (!isxdigit(static_cast<unsigned char>(in[i]))) return false;
    return true;
}

uint32_t recordCrc(const CloudV2Record& rec) {
    return crc32Calc(reinterpret_cast<const uint8_t*>(&rec), offsetof(CloudV2Record, crc32));
}

void clearRuntime() {
    memset(g_secret, 0, sizeof(g_secret));
    g_provisioned = false;
}
}

bool cloudV2CredentialsLoad() {
    clearRuntime();
    EEPROM.begin(CLOUD_V2_EEPROM_SIZE);
    CloudV2Record rec{};
    EEPROM.get(0, rec);
    EEPROM.end();

    if (rec.magic != CLOUD_V2_MAGIC || rec.version != CLOUD_V2_VERSION ||
        rec.secretLength != CLOUD_V2_SECRET_LEN ||
        rec.secret[CLOUD_V2_SECRET_LEN] != '\0' || rec.crc32 != recordCrc(rec)) return false;

    String secret(rec.secret);
    if (!secretValid(secret)) return false;
    memcpy(g_secret, rec.secret, sizeof(g_secret));
    g_provisioned = true;
    return true;
}

bool cloudV2CredentialsProvisioned() { return g_provisioned; }
String cloudV2CredentialsSecret() { return g_provisioned ? String(g_secret) : String(); }

bool cloudV2CredentialsSaveSecret(const String& input) {
    String secret = input;
    secret.trim();
    if (!secretValid(secret)) return false;

    CloudV2Record rec{};
    rec.magic = CLOUD_V2_MAGIC;
    rec.version = CLOUD_V2_VERSION;
    rec.secretLength = CLOUD_V2_SECRET_LEN;
    secret.toCharArray(rec.secret, sizeof(rec.secret));
    rec.crc32 = recordCrc(rec);

    EEPROM.begin(CLOUD_V2_EEPROM_SIZE);
    EEPROM.put(0, rec);
    const bool ok = EEPROM.commit();
    EEPROM.end();
    if (!ok) return false;

    memcpy(g_secret, rec.secret, sizeof(g_secret));
    g_provisioned = true;
    return true;
}

bool cloudV2CredentialsClear() {
    EEPROM.begin(CLOUD_V2_EEPROM_SIZE);
    for (size_t i = 0; i < CLOUD_V2_EEPROM_SIZE; ++i) EEPROM.write(i, 0xFF);
    const bool ok = EEPROM.commit();
    EEPROM.end();
    if (ok) clearRuntime();
    return ok;
}
#else
bool cloudV2CredentialsLoad() { return false; }
bool cloudV2CredentialsProvisioned() { return false; }
String cloudV2CredentialsSecret() { return String(); }
bool cloudV2CredentialsSaveSecret(const String&) { return false; }
bool cloudV2CredentialsClear() { return false; }
#endif
