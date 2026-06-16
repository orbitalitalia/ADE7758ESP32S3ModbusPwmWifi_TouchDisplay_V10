#include "globals.h"
#include <stdint.h>

ADE7758 meter;

// Tensiune: 196V real, 202.74V afișat
// new = old × (real / afisat)
float VRMS_SCALE_A = 0.0001293f;  // = 0.0001338 × (196/202.74)
float VRMS_SCALE_B = 0.0001293f;
float VRMS_SCALE_C = 0.0001293f;

// Curent: 3.85A real, 38.28A afișat
float IRMS_SCALE_A = 0.000000861f;  // = 0.000008562 × (3.85/38.28)
float IRMS_SCALE_B = 0.000000861f;
float IRMS_SCALE_C = 0.000000861f;

// Constanta hardware pura (calibrata cu CT secundar = 5A)
// Independenta de CT — folosita cu CT_Ratio = CT_Primary / CT_Secondary
float IRMS_SCALE_BASE_A = IRMS_SCALE_A * 5.0f;  // = 0.000004305f
float IRMS_SCALE_BASE_B = IRMS_SCALE_B * 5.0f;
float IRMS_SCALE_BASE_C = IRMS_SCALE_C * 5.0f;

bool pfUseRete = true;  // default: folosește Pf din Modbus când disponibil

SemaphoreHandle_t i2cMutex = nullptr;
SemaphoreHandle_t spiMutex = nullptr;   // 🔹 definiția reală

// ===== VARIABILE STRATEGIE TEHNICĂ =====
volatile bool g_netBusy = false;
volatile bool g_hardResetPending = false;
volatile bool g_modbusStateChanged = false;

// Add to globals.cpp
uint8_t modbusReconnectAttempts = 0;
// Add to globals.cpp
unsigned long lastModbusReconnectAttempt = 0;
bool modbusReconnectPending = false;

const uint32_t MODBUS_TIMEOUT_MS = 2000; // 2 secunde

uint32_t g_lastModbusTxMs = 0;
uint32_t g_lastModbusRxMs = 0;
volatile uint16_t g_modbusTid      = 0;

// ===== IMPLEMENTAREA FUNCȚIEI hardResetW5500 =====
// void hardResetW5500() {
//   DBG_PRINTLN("🔄 Hard reset W5500 (soft)...");
//   if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);

//   pinMode(W5500_RST, OUTPUT);
//   digitalWrite(W5500_RST, HIGH);   // idle sus
//   delay(1);
//   digitalWrite(W5500_RST, LOW);
//   delay(10);                       // 10 ms e suficient pt W5500
//   digitalWrite(W5500_RST, HIGH);

//   if (spiMutex) xSemaphoreGive(spiMutex);

//   delay(100);                      // settle 100 ms
//   DBG_PRINTLN("✅ W5500 reset complet!");
// }


// ===== VARIABILE DE REȚEA =====
ETHClass ETH;
WiFiClient client;
Preferences preferences;


SemaphoreHandle_t g_modbusMutex = nullptr;
volatile uint8_t  g_modbusFailCount = 0;
volatile bool     g_modbusResetRequest = false;
volatile uint32_t g_modbusJustConnectedAt = 0;

bool ethernetConnected = false;
bool modbusConnected   = false;
IPAddress MODBUS_SLAVE_IP(192,168,1,246);

TaskHandle_t TaskHandle_ModbusReconnect;
unsigned long lastReconnectAttempt = 0;

// ===== CONTROL MANUAL =====
bool manualControlEnabled = false;
const unsigned long manualControlAutoReset = 60000;
unsigned long manualControlTimeout = 0;

// 🔹 Control alimentare stepuri (mosfet P)
bool stepsEnabled = true;  // true = alimentare ON (implicit pornit)

int Step1 = 0;
bool Step2 = false, Step3 = false, Step4 = false;
float Watt = 0;
float PowerRete = 0.0f; // kW - Modbus Input 13 (Potenza rete)
float CosfiRete = 1.0f;

bool manualStep2 = false;
bool manualStep3 = false;
bool manualStep4 = false;
uint16_t pwm1 = 0;

// 🔥 VALORI CORECTE (sincronizate cu .ino)
float prez1 = 300.0f;    // Putere pentru PWM (W)
float prez2 = 125.0f;    // Putere pentru step2 (W)
float prez3 = 250.0f;    // Putere pentru step3 (W)
float prez4 = 250.0f;    // Putere pentru step4 (W)
int setpoint = 300;       // Setpoint în W (nu 20000!)

// În globals.cpp
uint32_t debug_whrA = 0;
uint32_t debug_whrA_prev = 0;
uint32_t debug_vahrA = 0;
uint32_t debug_vahrA_prev = 0;
int32_t debug_dW = 0;
int32_t debug_dVA = 0;
float debug_pfInstant = 0;

uint32_t debug_whrB = 0;
uint32_t debug_whrB_prev = 0;
uint32_t debug_vahrB = 0;
uint32_t debug_vahrB_prev = 0;
int32_t debug_dW_B = 0;
int32_t debug_dVA_B = 0;
float debug_pfInstant_B = 0;

uint32_t debug_whrC = 0;
uint32_t debug_whrC_prev = 0;
uint32_t debug_vahrC = 0;
uint32_t debug_vahrC_prev = 0;
int32_t debug_dW_C = 0;
int32_t debug_dVA_C = 0;
float debug_pfInstant_C = 0;


// ===== FUNCȚII GPIO =====
void updateStepGPIOs() {
  // 🔹 Control mosfet P (LOW = ON, HIGH = OFF)
  digitalWrite(STEPS_POWER_EN_PIN, stepsEnabled ? LOW : HIGH);
  
  // Control stepuri normale
  digitalWrite(STEP2_PIN, Step2 ? HIGH : LOW);
  digitalWrite(STEP3_PIN, Step3 ? HIGH : LOW);
  digitalWrite(STEP4_PIN, Step4 ? HIGH : LOW);
}

MbResult writeModbusRegister(uint16_t reg, uint16_t value) {
  // Basic checks
  if (!ethernetConnected || !client.connected()) {
    return MB_FAIL;
  }

  const uint16_t addr0 = MODBUS_ONE_BASED ? (reg - 1) : reg;
  const uint16_t tid   = g_modbusTid++;

  uint8_t req[12] = {
    (uint8_t)(tid >> 8), (uint8_t)tid, 0, 0, 0, 6,
    MODBUS_UNIT_ID, 0x06,
    (uint8_t)(addr0 >> 8), (uint8_t)(addr0 & 0xFF),
    (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)
  };

  // ⚠️ VERY IMPORTANT: Don't use mutex for Ethernet operations
  // Ethernet library handles its own thread safety
  
  // Check connection again right before sending
  if (!client.connected()) {
    return MB_FAIL;
  }

  // Send the request (non-blocking)
  int wrote = client.write(req, sizeof(req));
  
  if (wrote != (int)sizeof(req)) {
    // Check if connection was lost during write
    if (!client.connected()) {
      return MB_FAIL;
    }
    return MB_FAIL;
  }

  // Flush to ensure data is sent
  client.flush();
  
  // Update timestamps - assume success for keep-alive purposes
  g_lastModbusTxMs = millis();
  g_lastModbusRxMs = g_lastModbusTxMs;
  
  return MB_OK;
}


float voltageOffsetA = 0.0f;
float voltageOffsetB = 0.0f;
float voltageOffsetC = 0.0f;
float currentOffsetA = 0.0f;
float currentOffsetB = 0.0f;
float currentOffsetC = 0.0f;

bool factoryCalDone = false;  // ← ADAUGĂ ACEASTĂ LINIE




// ===== Timeout Modbus =====
void checkModbusTimeout() {
  if (client.connected()) {
    modbusConnected = (millis() - g_lastModbusRxMs) <= MODBUS_IDLE_MS; // OK doar cu răspuns recent
  } else {
    modbusConnected = false;
  }
}



bool reinitEthernetAndModbus() {
  DBG_PRINTLN("🔄 Reinit Ethernet + Modbus...");
  // 👉 Doar dacă ETH e mortal (fără IP) acceptăm hard reset:
  bool needHard = (ETH.localIP() == IPAddress(0,0,0,0)) || !ethernetConnected;
  if (needHard) {
    //hardResetW5500();              // altfel, sari peste
    delay(100);
  }

  if (!ETH.beginSPI(W5500_MISO, W5500_MOSI, W5500_SCLK,
                    W5500_CS, W5500_RST, W5500_IRQ)) {
    DBG_PRINTLN("❌ Error reinitializing W5500");
    ethernetConnected = false;
    return false;
  }

  // (restul rămâne la fel)
}

#include <SPI.h>

// citește registru socket W5500
// Low-level: NU ia mutex. Presupune că apelantul deține deja spiMutex.
static uint8_t w5500_read_nolock(uint16_t addr, uint8_t block) {
  uint8_t data = 0;
  SPI.beginTransaction(SPISettings(14000000, MSBFIRST, SPI_MODE0));
  digitalWrite(W5500_CS, LOW);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr & 0xFF);
  SPI.transfer((block << 3) | 0x00);
  data = SPI.transfer(0x00);
  digitalWrite(W5500_CS, HIGH);
  SPI.endTransaction();
  return data;
}

// Wrapper: DOAR aici, la nevoie, luăm mutexul rapid.
uint8_t w5500_read(uint16_t addr, uint8_t block) {
  uint8_t out = 0;
  if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
    out = w5500_read_nolock(addr, block);
    xSemaphoreGive(spiMutex);
  } else {
    DBG_PRINTLN("⚠️ w5500_read skipped (SPI busy)");
  }
  return out;
}


void printW5500Sockets() {
  if (!(spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50)))) {
    DBG_PRINTLN("⚠️ skip sockets dump (SPI busy)");
    return;
  }
  for (int s = 0; s < 8; s++) {
    uint16_t base = 0x400 * s;
    uint8_t sr = w5500_read_nolock(0x0003 + base, 0x01); // ✅ fără lock aici
    Serial.print("Socket "); Serial.print(s);
    Serial.print(" = 0x"); Serial.println(sr, HEX);
  }
  xSemaphoreGive(spiMutex);
}
void ModbusReconnectTask(void *pv) {
  for (;;) {
    if (ethernetConnected && ETH.linkUp() && !client.connected()) {
      DBG_PRINTLN("🔄 Trying Modbus reconnect...");
      if (client.connect(MODBUS_SLAVE_IP, MODBUS_PORT)) {
        DBG_PRINTLN("✅ Modbus reconnected!");
        modbusConnected = true;
        g_lastModbusTxMs = millis();
      } else {
        modbusConnected = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // verifică la 5s
  }
}
float CT_Primary = 200.0f;
float CT_Secondary = 1.0f;

// ═══════════════════════════════════════════════════════════
// 🔹 FILTRARE POWER FACTOR pentru stabilitate
// ═══════════════════════════════════════════════════════════

// Variabile statice pentru filtrare
static float pfFilteredA = 1.0f;
static float pfFilteredB = 1.0f;
static float pfFilteredC = 1.0f;

// Constante filtrare
#define PF_FILTER_ALPHA   0.2f    // 0.1-0.3 = smooth, 0.4-0.6 = rapid
#define PF_MIN_VA         20.0f   // VA minim pentru calcul valid (20VA)

float getFilteredPowerFactor(int phase) {
    // Citește puteri (consecutiv rapid pentru sincronizare)
    float watt = 0, va = 0;
    
    if (phase == PHASE_A) {
       watt = WattA;
va = VA_a;
    } else if (phase == PHASE_B) {
       watt = WattB;
va = VA_b;
    } else if (phase == PHASE_C) {
        watt = WattC;
va = VA_c;
    }
    
    // Calcul PF instantaneu
    float pfInstant = 1.0f;
    if (va > PF_MIN_VA) {
        pfInstant = watt / va;
        // Clamp la [-1.0, 1.0]
        if (pfInstant > 1.0f) pfInstant = 1.0f;
        if (pfInstant < -1.0f) pfInstant = -1.0f;
    }
    
    // Filtrare low-pass: PF_new = alpha * PF_instant + (1-alpha) * PF_old
    float* pfFiltered = (phase == PHASE_A) ? &pfFilteredA : 
                        (phase == PHASE_B) ? &pfFilteredB : &pfFilteredC;
    
    *pfFiltered = PF_FILTER_ALPHA * pfInstant + (1.0f - PF_FILTER_ALPHA) * (*pfFiltered);
    
    return *pfFiltered;
}
