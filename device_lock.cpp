#include "device_lock.h"
#include <esp_system.h>      // ← fix #1: compatibil toate SDK
#include "mbedtls/md.h"
#include <Wire.h>

#define FRAM_I2C_ADDR       0x50
#define LOCK_SIG_ADDR       0x1FC0
#define LOCK_MARKER_ADDR    0x1FE0
#define LOCK_MAC_ADDR       0x1FE4
#define LOCK_COUNTER_ADDR   0x1FEA
#define MAGIC_MARKER        0xFEED1234

static const uint8_t SECRET_KEY[32] = {
0x99, 0x74, 0x8C, 0x66, 0xC7, 0x8A, 0x05, 0xB9, 0x14, 0xFD, 0x1E, 0xCE, 0x8B, 0x94, 0x13, 0x78, 0x0D, 0xD5, 0xF1, 0xC2, 0xFF, 0x62, 0x15, 0x39, 0xBC, 0x55, 0xF9, 0xF9, 0x6E, 0x02, 0x43, 0x48
};

// ─── FRAM R/W ─────────────────────────────────────────────────────────────────
static bool fram_write(uint16_t addr, const uint8_t* data, uint16_t len) {
    Wire.beginTransmission(FRAM_I2C_ADDR);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    for (uint16_t i = 0; i < len; i++) Wire.write(data[i]);
    return (Wire.endTransmission() == 0);
}

static bool fram_read(uint16_t addr, uint8_t* data, uint16_t len) {
    Wire.beginTransmission(FRAM_I2C_ADDR);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    
    // ← fix #2: size_t cast corect
    size_t received = Wire.requestFrom(FRAM_I2C_ADDR, (size_t)len);
    if (received != len) return false;
    
    for (uint16_t i = 0; i < len; i++) data[i] = Wire.read();
    return true;
}

static bool fram_write_u32(uint16_t addr, uint32_t val) {
    return fram_write(addr, (uint8_t*)&val, 4);
}

static uint32_t fram_read_u32(uint16_t addr) {
    uint32_t val = 0;
    fram_read(addr, (uint8_t*)&val, 4);
    return val;
}

// ─── HMAC-SHA256 cu verificare return codes ───────────────────────────────────
static bool compute_signature(const uint8_t* mac, uint8_t* out_hash) {
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    
    // ← fix #3: verifica init
    if (info == nullptr) return false;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    mbedtls_md_hmac_starts(&ctx, SECRET_KEY, 32);
    mbedtls_md_hmac_update(&ctx, mac, 6);
    mbedtls_md_hmac_finish(&ctx, out_hash);
    mbedtls_md_free(&ctx);
    return true;
}

static bool safe_compare(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (a[i] ^ b[i]);
    return (diff == 0);
}

// ─── MAIN ─────────────────────────────────────────────────────────────────────
bool deviceLock_init() {
    Serial.println("[LOCK] === Device Lock Check ===");
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("[LOCK] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    uint8_t computed[32];
    if (!compute_signature(mac, computed)) {       // ← fix #3: verifica compute
        Serial.println("[LOCK] EROARE: compute_signature failed!");
        return false;
    }
    
    uint32_t marker = fram_read_u32(LOCK_MARKER_ADDR);
    
    if (marker != MAGIC_MARKER) {
        Serial.println("[LOCK] Prima pornire - inregistrez dispozitivul...");
        
        bool ok = true;
        ok &= fram_write(LOCK_SIG_ADDR, computed, 32);
        ok &= fram_write(LOCK_MAC_ADDR, mac, 6);
        ok &= fram_write_u32(LOCK_COUNTER_ADDR, 1);
        ok &= fram_write_u32(LOCK_MARKER_ADDR, MAGIC_MARKER);  // ← ultimul!
        
        if (!ok) {
            Serial.println("[LOCK] ERORE Critica");
            return false;
        }
        
        Serial.println("[LOCK] Dispozitiv inregistrat cu succes.");
        return true;
    }
    
    uint8_t stored[32];
    if (!fram_read(LOCK_SIG_ADDR, stored, 32)) {
        Serial.println("[LOCK] EROARE: ");
        return false;
    }
    
    if (!safe_compare(computed, stored, 32)) {
        Serial.println("[LOCK] CLONA DETECTATA ");
        return false;
    }
    
    uint32_t boots = fram_read_u32(LOCK_COUNTER_ADDR);
    fram_write_u32(LOCK_COUNTER_ADDR, boots + 1);
    Serial.printf("[LOCK] Verificare OK. Boot #%lu\n", boots + 1);
    return true;
}

bool deviceLock_check() {
    uint8_t mac[6], computed[32], stored[32];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (!compute_signature(mac, computed)) return false;
    if (!fram_read(LOCK_SIG_ADDR, stored, 32)) return false;
    return safe_compare(computed, stored, 32);
}

void deviceLock_erase() {
    // ← fix #4: sterge complet, marker ultimul
    uint8_t zeros[32] = {0};
    fram_write(LOCK_SIG_ADDR, zeros, 32);
    fram_write(LOCK_MAC_ADDR, zeros, 6);
    fram_write_u32(LOCK_COUNTER_ADDR, 0);
    fram_write_u32(LOCK_MARKER_ADDR, 0);
    Serial.println("[LOCK] Inregistrare stearsa complet.");
}