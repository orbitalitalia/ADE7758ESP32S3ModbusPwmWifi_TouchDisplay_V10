#include <Arduino.h> 
#include "NVRAM.h"
#include "globals.h"
#include "NVRAM.h"
#include <math.h>

// ═══════════════════════════════════════════════════════════════
// FUNZIONI DI BASE - LETTURA/SCRITTURA BYTE
// ═══════════════════════════════════════════════════════════════

void nvramWriteByte(uint16_t addr, uint8_t value) {
    Wire.beginTransmission(EEPROM_ADDRESS);
    Wire.write((uint8_t)(addr >> 8));   // MSB
    Wire.write((uint8_t)(addr & 0xFF)); // LSB
    Wire.write(value);
    Wire.endTransmission();
    delay(5);  // EEPROM write cycle time
}

uint8_t nvramReadByte(uint16_t addr) {
    Wire.beginTransmission(EEPROM_ADDRESS);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)EEPROM_ADDRESS, (uint8_t)1);
    if (Wire.available()) {
        return Wire.read();
    }
    return 0xFF; // Valore di default se la lettura fallisce
}

bool smartWriteByte(uint16_t addr, uint8_t value) {
    uint8_t oldValue = nvramReadByte(addr);
    if (oldValue != value) {
        nvramWriteByte(addr, value);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// FUNZIONI FLOAT (4 bytes)
// ═══════════════════════════════════════════════════════════════

static bool eepromWaitReady(uint32_t timeoutMs = 25) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    Wire.beginTransmission(EEPROM_ADDRESS);
    uint8_t err = Wire.endTransmission();   // dacă e gata, va da ACK => err==0
    if (err == 0) return true;
    delay(1);
  }
  return false;
}

void nvramWriteFloat(uint16_t addr, float value) {
  uint8_t *p = (uint8_t*)&value;

  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return; // sau log error
  }

  Wire.beginTransmission(EEPROM_ADDRESS);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)(addr & 0xFF));
  Wire.write(p, sizeof(float));

  uint8_t err = Wire.endTransmission();
  if (err == 0) {
    // așteaptă finalizarea write-cycle intern
    eepromWaitReady(25);
  }

  if (i2cMutex) xSemaphoreGive(i2cMutex);
}



float nvramReadFloat(uint16_t addr) {
    float value = 0.0f;

    if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return NAN;
    }

    Wire.beginTransmission(EEPROM_ADDRESS);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)(addr & 0xFF));
    if (Wire.endTransmission(false) != 0) {
        if (i2cMutex) xSemaphoreGive(i2cMutex);
        return NAN;
    }

    uint8_t got = Wire.requestFrom((uint8_t)EEPROM_ADDRESS, (uint8_t)sizeof(float));
    if (got != sizeof(float)) {
        if (i2cMutex) xSemaphoreGive(i2cMutex);
        return NAN;
    }

    uint8_t *p = (uint8_t*)&value;
    for (uint8_t i = 0; i < sizeof(float); i++) p[i] = Wire.read();

    if (i2cMutex) xSemaphoreGive(i2cMutex);
    return value;
}


bool smartWriteFloat(uint16_t addr, float value) {
    float oldValue = nvramReadFloat(addr);

    // dacă nu am putut citi vechiul sau e invalid, scrie direct
    if (!isfinite(oldValue) || !isfinite(value)) {
        nvramWriteFloat(addr, value);
        return true;
    }

    if (fabsf(oldValue - value) > 0.01f) {
        nvramWriteFloat(addr, value);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// FUNZIONI INT (4 bytes)
// ═══════════════════════════════════════════════════════════════

void nvramWriteInt(uint16_t addr, int value) {
    uint8_t *p = (uint8_t*)&value;
    for (uint8_t i = 0; i < sizeof(int); i++) {
        Wire.beginTransmission(EEPROM_ADDRESS);
        Wire.write((uint8_t)((addr + i) >> 8));
        Wire.write((uint8_t)((addr + i) & 0xFF));
        Wire.write(p[i]);
        Wire.endTransmission();

    }
}

int nvramReadInt(uint16_t addr) {
    int value = 0;
    uint8_t *p = (uint8_t*)&value;
    for (uint8_t i = 0; i < sizeof(int); i++) {
        Wire.beginTransmission(EEPROM_ADDRESS);
        Wire.write((uint8_t)((addr + i) >> 8));
        Wire.write((uint8_t)((addr + i) & 0xFF));
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)EEPROM_ADDRESS, (uint8_t)1);
        if (Wire.available()) {
            p[i] = Wire.read();
        }
    }
    return value;
}

bool smartWriteInt(uint16_t addr, int value) {
    int oldValue = nvramReadInt(addr);
    if (oldValue != value) {
        nvramWriteInt(addr, value);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// ✅ FUNZIONI IP MODBUS (salvato in NVRAM I2C, non Preferences)
// ═══════════════════════════════════════════════════════════════

void loadModbusIPFromNVRAM() {
    // Leggi i 4 byte dell'IP da NVRAM
    uint8_t ip0 = nvramReadByte(ADDR_MODBUS_IP + 0);
    uint8_t ip1 = nvramReadByte(ADDR_MODBUS_IP + 1);
    uint8_t ip2 = nvramReadByte(ADDR_MODBUS_IP + 2);
    uint8_t ip3 = nvramReadByte(ADDR_MODBUS_IP + 3);
    
    // Verifica se l'IP è valido (non tutto 0xFF = EEPROM vuota)
    if (ip0 == 0xFF && ip1 == 0xFF && ip2 == 0xFF && ip3 == 0xFF) {
        DBG_PRINTLN("ℹ️ Nessun IP Modbus salvato in NVRAM, uso default");
        // IP rimane quello di default da globals.cpp
    } else if (ip0 == 0 && ip1 == 0 && ip2 == 0 && ip3 == 0) {
        DBG_PRINTLN("⚠️ IP Modbus in NVRAM è 0.0.0.0, uso default");
    } else {
        // IP valido trovato
        MODBUS_SLAVE_IP = IPAddress(ip0, ip1, ip2, ip3);
        DBG_PRINT("✅ IP Modbus caricato da NVRAM: ");
        DBG_PRINTLN(MODBUS_SLAVE_IP);
    }
    
    DBG_PRINT("📌 IP finale Modbus: ");
    DBG_PRINTLN(MODBUS_SLAVE_IP);
}

void saveModbusIPToNVRAM() {
    bool changed = false;
    
    // Salva ogni ottetto dell'IP
    changed |= smartWriteByte(ADDR_MODBUS_IP + 0, MODBUS_SLAVE_IP[0]);
    changed |= smartWriteByte(ADDR_MODBUS_IP + 1, MODBUS_SLAVE_IP[1]);
    changed |= smartWriteByte(ADDR_MODBUS_IP + 2, MODBUS_SLAVE_IP[2]);
    changed |= smartWriteByte(ADDR_MODBUS_IP + 3, MODBUS_SLAVE_IP[3]);
    
    if (changed) {
        DBG_PRINT("💾 IP Modbus salvato in NVRAM: ");
        DBG_PRINTLN(MODBUS_SLAVE_IP);
    } else {
        DBG_PRINTLN("ℹ️ IP Modbus non modificato, nessuna scrittura");
    }
}

// ═══════════════════════════════════════════════════════════════
// CARICAMENTO IMPOSTAZIONI GENERALI
// ═══════════════════════════════════════════════════════════════

void loadSettingsFromNVRAM() {
    DBG_PRINTLN("📖 Caricamento impostazioni da NVRAM...");
    int f = nvramReadInt(ADDR_FACTORY_CAL_DONE);
factoryCalDone = (f == 1);
if (f < 0) factoryCalDone = false;

    // Calibrazione generale
    voltageFactor = nvramReadFloat(ADDR_VOLTAGE_FACTOR);
    currentFactor = nvramReadFloat(ADDR_CURRENT_FACTOR);
    totalEnergy   = nvramReadFloat(ADDR_TOTAL_ENERGY);
    KWh           = nvramReadFloat(ADDR_KWH);
    setpoint      = nvramReadInt(ADDR_SETPOINT);
    prez1 = nvramReadInt(ADDR_PREZ1);
    prez2 = nvramReadInt(ADDR_PREZ2);
    prez3 = nvramReadInt(ADDR_PREZ3);
    prez4 = nvramReadInt(ADDR_PREZ4);
    
    // ✅ OFFSET (NUOVI)
    voltageOffsetA = nvramReadFloat(ADDR_VOLTAGE_OFFSET_A);
    voltageOffsetB = nvramReadFloat(ADDR_VOLTAGE_OFFSET_B);
    voltageOffsetC = nvramReadFloat(ADDR_VOLTAGE_OFFSET_C);
    currentOffsetA = nvramReadFloat(ADDR_CURRENT_OFFSET_A);
    currentOffsetB = nvramReadFloat(ADDR_CURRENT_OFFSET_B);
    currentOffsetC = nvramReadFloat(ADDR_CURRENT_OFFSET_C);

    CT_Primary = nvramReadFloat(ADDR_CT_PRIMARY);
    CT_Secondary = nvramReadFloat(ADDR_CT_SECONDARY);

    // 🔹 Alimentare stepuri
    uint8_t stepsEnabledByte = nvramReadByte(ADDR_STEPS_ENABLED);
    if (stepsEnabledByte == 0xFF) {
        // NVRAM niciodată scris -> default ON
        stepsEnabled = true;
    } else {
        stepsEnabled = (stepsEnabledByte == 1);
    }

        // 🔥 ADAUGĂ ASTA - încărcare IP Modbus din NVRAM
    loadModbusIPFromNVRAM();


    // Fallback valori di default
    if (prez1 < 0) prez1 = 1200;
    if (prez2 < 0) prez2 = 1000;
    if (prez3 < 0) prez3 = 1000;
    if (prez4 < 0) prez4 = 1000;
    if (isnan(voltageFactor) || voltageFactor == 0.0) voltageFactor = 1.0;
    if (isnan(currentFactor) || currentFactor == 0.0) currentFactor = 1.0;
    if (isnan(totalEnergy)) totalEnergy = 0.0;
    if (isnan(KWh)) KWh = 0.0;
    
    // Fallback offset (se NaN, setta a zero)
    if (isnan(voltageOffsetA)) voltageOffsetA = 0.0f;
    if (isnan(voltageOffsetB)) voltageOffsetB = 0.0f;
    if (isnan(voltageOffsetC)) voltageOffsetC = 0.0f;
    if (isnan(currentOffsetA)) currentOffsetA = 0.0f;
    if (isnan(currentOffsetB)) currentOffsetB = 0.0f;
    if (isnan(currentOffsetC)) currentOffsetC = 0.0f;
    if (isnan(CT_Primary) || CT_Primary <= 0) CT_Primary = 200.0f;
    if (isnan(CT_Secondary) || CT_Secondary <= 0) CT_Secondary = 1.0f;

    
    DBG_PRINTLN("✅ Impostazioni caricate da NVRAM");
    DBG_PRINTf("   voltageFactor: %.3f\n", voltageFactor);
    DBG_PRINTf("   currentFactor: %.3f\n", currentFactor);
    DBG_PRINTf("   setpoint: %d W\n", setpoint);
    DBG_PRINTf("   Offset V_A/B/C: %.2f / %.2f / %.2f\n", voltageOffsetA, voltageOffsetB, voltageOffsetC);
    DBG_PRINTf("   Offset I_A/B/C: %.2f / %.2f / %.2f\n", currentOffsetA, currentOffsetB, currentOffsetC);
    DBG_PRINTf("   Prez1..4: %d / %d / %d / %d\n", prez1, prez2, prez3, prez4);

}

// ═══════════════════════════════════════════════════════════════
// SALVATAGGIO IMPOSTAZIONI GENERALI
// ═══════════════════════════════════════════════════════════════

void saveSettingsToNVRAM() {
    DBG_PRINTLN("💾 Salvataggio impostazioni in NVRAM.");

    bool changed = false;

    // Calibrazione generale
    changed |= smartWriteFloat(ADDR_VOLTAGE_FACTOR, voltageFactor);
    changed |= smartWriteFloat(ADDR_CURRENT_FACTOR, currentFactor);
    changed |= smartWriteFloat(ADDR_TOTAL_ENERGY, totalEnergy);
    changed |= smartWriteFloat(ADDR_KWH, KWh);
    changed |= smartWriteInt(ADDR_SETPOINT, setpoint);
    changed |= smartWriteInt(ADDR_PREZ1, prez1);
    changed |= smartWriteInt(ADDR_PREZ2, prez2);
    changed |= smartWriteInt(ADDR_PREZ3, prez3);
    changed |= smartWriteInt(ADDR_PREZ4, prez4);

changed |= smartWriteFloat(ADDR_VOLTAGE_OFFSET_A, voltageOffsetA);
changed |= smartWriteFloat(ADDR_VOLTAGE_OFFSET_B, voltageOffsetB);
changed |= smartWriteFloat(ADDR_VOLTAGE_OFFSET_C, voltageOffsetC);
changed |= smartWriteFloat(ADDR_CURRENT_OFFSET_A, currentOffsetA);
changed |= smartWriteFloat(ADDR_CURRENT_OFFSET_B, currentOffsetB);
changed |= smartWriteFloat(ADDR_CURRENT_OFFSET_C, currentOffsetC);

    // Salvează flag-ul
    changed |= smartWriteInt(ADDR_FACTORY_CAL_DONE, factoryCalDone ? 1 : 0);

    changed |= smartWriteFloat(ADDR_CT_PRIMARY, CT_Primary);
    changed |= smartWriteFloat(ADDR_CT_SECONDARY, CT_Secondary);

    // 🔹 Alimentare stepuri
    changed |= smartWriteByte(ADDR_STEPS_ENABLED, stepsEnabled ? 1 : 0);

    saveModbusIPToNVRAM();

    if (changed) {
        DBG_PRINTLN("✅ Impostazioni salvate in NVRAM");
    } else {
        DBG_PRINTLN("ℹ️ Nessuna modifica da salvare");
    }
}

