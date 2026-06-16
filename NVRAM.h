#ifndef NVRAM_H
#define NVRAM_H

#include <Arduino.h>
#include <Wire.h>

// Adresa I2C a EEPROM-ului
#define EEPROM_ADDRESS 0x50

// ═══════════════════════════════════════════════════════════════
// ADRESE NVRAM (I2C EEPROM 24C256 - 32KB)
// ═══════════════════════════════════════════════════════════════

// Calibrare generală (adrese existente)
#define ADDR_VOLTAGE_FACTOR   0x0000  // 4 bytes (float)
#define ADDR_CURRENT_FACTOR   0x0004  // 4 bytes (float)
#define ADDR_TOTAL_ENERGY     0x0008  // 4 bytes (float)
#define ADDR_KWH              0x000C  // 4 bytes (float)
#define ADDR_SETPOINT         0x0010  // 4 bytes (int)
#define ADDR_PREZ1            0x0014  // 4 bytes (int)
#define ADDR_PREZ2            0x0018  // 4 bytes (int)
#define ADDR_PREZ3            0x001C  // 4 bytes (int)
#define ADDR_PREZ4            0x0020  // 4 bytes (int)

#define ADDR_FACTORY_CAL_DONE  0x0220


#define ADDR_CT_PRIMARY    0x0048  // ← ADAUGĂ
#define ADDR_CT_SECONDARY  0x004C  // ← ADAUGĂ

// ✅ OFFSET-URI (ADRESE NOI)
#define ADDR_VOLTAGE_OFFSET_A 0x0030  // 4 bytes (float)
#define ADDR_VOLTAGE_OFFSET_B 0x0034  // 4 bytes (float)
#define ADDR_VOLTAGE_OFFSET_C 0x0038  // 4 bytes (float)
#define ADDR_CURRENT_OFFSET_A 0x003C  // 4 bytes (float)
#define ADDR_CURRENT_OFFSET_B 0x0040  // 4 bytes (float)
#define ADDR_CURRENT_OFFSET_C 0x0044  // 4 bytes (float)

// ✅ IP MODBUS (4 bytes - un byte per ogni ottetto)
#define ADDR_MODBUS_IP        0x0050  // 4 bytes (uint8_t x4)

// 🔹 Control alimentare stepuri (1 byte - bool)
#define ADDR_STEPS_ENABLED    0x0054  // 1 byte (bool)

// Rezervă spațiu pentru viitor
#define ADDR_RESERVED_START   0x0060  // Spazio riservato per future espansioni

// ═══════════════════════════════════════════════════════════════
// DECLARAȚII FUNCȚII
// ═══════════════════════════════════════════════════════════════

// Funcții de bază citire/scriere
void nvramWriteFloat(uint16_t addr, float value);
float nvramReadFloat(uint16_t addr);
void nvramWriteInt(uint16_t addr, int value);
int nvramReadInt(uint16_t addr);
void nvramWriteByte(uint16_t addr, uint8_t value);
uint8_t nvramReadByte(uint16_t addr);

// Funcții smart write (scrie doar dacă valoarea s-a schimbat)
bool smartWriteFloat(uint16_t addr, float value);
bool smartWriteInt(uint16_t addr, int value);
bool smartWriteByte(uint16_t addr, uint8_t value);

// Funcții salvare/citire grup de setări
void loadSettingsFromNVRAM();
void saveSettingsToNVRAM();

// Funcții specifice pentru IP Modbus
void loadModbusIPFromNVRAM();
void saveModbusIPToNVRAM();

#endif // NVRAM_H