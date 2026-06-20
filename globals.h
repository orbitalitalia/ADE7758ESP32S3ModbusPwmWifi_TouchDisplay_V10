#ifndef GLOBALS_H
#define GLOBALS_H

// Include-urile existente...
#include <stdint.h>
#include <WiFi.h>
#include "ETHClass.h"
#include <IPAddress.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ADE7758.h"

extern ADE7758 meter;

#define FW_VERSION "10.2"

extern char fwVersionString[16];
void updateFirmwareVersionString();

extern bool pfUseRete;  // true=Pf Rete, false=Pf ADE calculat

extern SemaphoreHandle_t i2cMutex;
extern SemaphoreHandle_t spiMutex;   // 🔹 adaugă asta

// Offset-uri pentru calibrare
extern float voltageOffsetA;
extern float voltageOffsetB;
extern float voltageOffsetC;
extern float currentOffsetA;
extern float currentOffsetB;
extern float currentOffsetC;

extern bool factoryCalDone;

extern float IRMS_SCALE_A;
extern float IRMS_SCALE_B;
extern float IRMS_SCALE_C;

extern float IRMS_SCALE_BASE_A;
extern float IRMS_SCALE_BASE_B;
extern float IRMS_SCALE_BASE_C;

extern float VRMS_SCALE_A;
extern float VRMS_SCALE_B;
extern float VRMS_SCALE_C;



// Add to globals.h
#define MODBUS_RECONNECT_INTERVAL 5000    // 5 seconds between attempts
#define MODBUS_MAX_RECONNECT_ATTEMPTS 0   // 0 = infinite retries

// Add to globals.h
extern bool modbusConnectionInProgress;
extern uint32_t modbusLastActivityTime;
extern volatile bool g_modbusStateChanged;

extern uint8_t modbusReconnectAttempts;
// Add to globals.h
extern unsigned long lastModbusReconnectAttempt;
extern bool modbusReconnectPending;

// --- Modbus timeouts ---
#ifndef MODBUS_RX_TIMEOUT_MS
#define MODBUS_RX_TIMEOUT_MS 3000   // valid RX required in last 3s for UI green
#endif
#ifndef MODBUS_IDLE_MS
#define MODBUS_IDLE_MS MODBUS_RX_TIMEOUT_MS
#endif

// --- Modbus timeouts/status ---
enum MbResult { MB_OK = 1, MB_FAIL = 0, MB_SKIPPED = -1 };

// -------- Modbus session policy --------
#ifndef MODBUS_STICKY_SESSION
#define MODBUS_STICKY_SESSION 1   // 1 = NU închidem TCP pe idle; doar pe TCP down sau manual
#endif
#ifndef MODBUS_GIVEUP_MS
#define MODBUS_GIVEUP_MS 0        // 0 = fără „force close” pe idle prelungit
#endif
#ifndef MODBUS_CONNECT_GRACE_MS
#define MODBUS_CONNECT_GRACE_MS 1500    // wait after connect before first frame
#endif
#ifndef MODBUS_BACKOFF_MIN_MS
#define MODBUS_BACKOFF_MIN_MS  1000     // initial reconnect backoff
#endif
#ifndef MODBUS_BACKOFF_MAX_MS
#define MODBUS_BACKOFF_MAX_MS  30000    // cap backoff at 30s
#endif

extern uint32_t g_lastModbusTxMs;
extern uint32_t g_lastModbusRxMs;

// Single write API (tri-state). No other function should touch client except ModbusTask.
MbResult writeModbusRegister(uint16_t reg, uint16_t value);


// ===== VARIABILE PENTRU STRATEGIA TEHNICĂ =====
extern volatile bool g_netBusy;           // TRUE când se face connect() - protejează SPI
extern volatile bool g_hardResetPending;  // TRUE când trebuie hard reset W5500

void checkModbusTimeout();
bool reinitEthernetAndModbus();

// ===== VARIABILE EXISTENTE =====
extern SemaphoreHandle_t g_modbusMutex;
extern volatile uint16_t g_modbusTid;
extern volatile uint8_t  g_modbusFailCount;
extern volatile bool     g_modbusResetRequest;
extern volatile uint32_t g_modbusJustConnectedAt;

extern IPAddress MODBUS_SLAVE_IP;
extern Preferences preferences;
extern WiFiClient client;


extern bool manualControlEnabled;
extern unsigned long manualControlTimeout;
extern const unsigned long manualControlAutoReset;

// 🔹 Control alimentare stepuri (mosfet P)
extern bool stepsEnabled;  // true = alimentare ON, false = alimentare OFF

extern ETHClass ETH;
extern bool manualStep2, manualStep3, manualStep4;
extern double v, v_a, v_b, v_c;
extern double c, c_a, c_b, c_c;
extern float Watt, WattA, WattB, WattC;
extern float PowerRete; // kW - citita via Modbus Input 13 (Potenza rete)
// Potenza rete (citita via Modbus, Input Register 13)
extern float CosfiRete;
extern volatile bool g_exportSafetyActive;
extern float g_exportExcessW;
extern float pGenAtModbusW;
extern float pReteAtModbusW;
extern float pDumpRealW;
extern float pDumpExpectedW;
extern float pDumpErrorW;
extern float modbusPowerCorrectionW;
extern float gridPowerErrorW;
extern bool modbusCorrectionEnabled;

extern uint32_t lastPowerReteUpdateMs;

extern bool powerReteValid;
extern bool dumpDiagnosticEnabled;
extern bool dumpLoadMismatch;

extern bool step2DiagFault;
extern bool step3DiagFault;
extern bool step4DiagFault;
extern float VA_a, VA_b, VA_c;
extern float Var_a, Var_b, Var_c;
extern float PfA, PfB, PfC, Pf;
extern double Frequency;
extern uint16_t pwm1;

extern float prez1, prez2, prez3, prez4;
extern int Step1;
extern bool Step2, Step3, Step4;

extern float voltageFactor, currentFactor, totalEnergy, KWh;
extern float CT_Primary, CT_Secondary;
extern int setpoint;
extern bool resetEnergyRequested;

extern SemaphoreHandle_t spiMutex;


extern bool ethernetConnected;
extern bool modbusConnected;
extern bool w5500Available;
extern TaskHandle_t TaskHandle_ModbusReconnect;
extern unsigned long lastReconnectAttempt;

// Pentru debugging OTA
extern uint32_t debug_whrA;
extern uint32_t debug_whrB;
extern uint32_t debug_whrC;
extern uint32_t debug_vahrA;
extern uint32_t debug_vahrB;
extern uint32_t debug_vahrC;


// ===== DECLARAȚII FUNCȚII PENTRU STRATEGIA TEHNICĂ =====
void hardResetW5500();                    // Hard reset W5500 + recreare stack Ethernet
void startModbusReconnectTask();          // Pornește task-ul de reconectare

void updateOLED();
void tryReconnectModbus();
void startWebServer();
void printW5500Sockets();


// ===== DECLARAȚII FUNCȚII EXISTENTE =====
// ===== DECLARAȚII TASK-URI =====
void modbusTickTask(void *pv);   // Task pentru keep-alive Modbus
void ModbusReconnectTask(void *pv);     // <— ADD
void updatePWMFromStep1();
void setPWM(int value);
void updateStepGPIOs();
static inline void requestModbusReconnect() { g_modbusResetRequest = true; }

// ===== CONSTANTE MODBUS =====
#ifndef MODBUS_PORT
  #define MODBUS_PORT 502
#endif

#ifndef MODBUS_UNIT_ID
  #define MODBUS_UNIT_ID 1
#endif

#ifndef MODBUS_ONE_BASED
  #define MODBUS_ONE_BASED 0
#endif

#ifndef MODBUS_REG_WATT
  #define MODBUS_REG_WATT 2017
#endif

#ifndef MODBUS_REG_STAY_ALIVE
  #define MODBUS_REG_STAY_ALIVE 2021
#endif

#ifndef MODBUS_RETRY_INTERVAL
  #define MODBUS_RETRY_INTERVAL 5000
#endif

// ===== CONSTANTE PENTRU STRATEGIA TEHNICĂ =====
#ifndef MODBUS_MAX_CONSECUTIVE_FAILURES
  #define MODBUS_MAX_CONSECUTIVE_FAILURES 5  // Hard reset după 5 eșecuri
#endif

// ===== PINII W5500 (dacă nu sunt definiți) =====
#ifndef W5500_MISO
  #define W5500_MISO 12
#endif
#ifndef W5500_MOSI  
  #define W5500_MOSI 11
#endif
#ifndef W5500_SCLK
  #define W5500_SCLK 13
#endif
#ifndef W5500_CS
  #define W5500_CS 14
#endif
#ifndef W5500_RST
  #define W5500_RST 9
#endif
#ifndef W5500_IRQ
  #define W5500_IRQ 10
#endif

// ===== ALTE CONSTANTE =====
#define FACTOR_VRMS (0.089466)
#define FACTOR_IRMS (4.168 / 1190.0)
#define FACTOR_ENERGY_ACT (0.0000509 / 3600.0)
#define FACTOR_ENERGY_REA (0.0000509)
#define FACTOR_POWER_ACT (0.00001)
#define FACTOR_POWER_VA  0.00001
#define FREQ_CAL 16.14f


#define MIN_IRMS 0.01
#define MIN_POWER 1.0

#define PHASE_A 0
#define PHASE_B 1
#define PHASE_C 2

#define STEP2_PIN 16
#define STEP3_PIN 17
#define STEP4_PIN 18
#define STEPS_POWER_EN_PIN 38  // 🔹 Pin pentru mosfet P (LOW = ON, HIGH = OFF)

#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
  #define DBG_PRINT(x)    Serial.print(x)
  #define DBG_PRINTLN(x)  Serial.println(x)
  #define DBG_PRINTf(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTf(...)
#endif

#define ADE_IRQ_PIN 15

// Funcție filtrare Power Factor
float getFilteredPowerFactor(int phase);

#endif // GLOBALS_H