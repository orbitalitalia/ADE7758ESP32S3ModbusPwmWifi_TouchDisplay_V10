// V9.0 ADE7758 with ESP32 S3 Devmodule - VERSIUNEA CORECTATĂ MODBUS + WiFi + OTA + anticlonare
// Model functional calibrat pe faza A,B si C
// SPI funcțional - problema cu dummy byte rezolvată
// Citiri rapide - fără delay-uri inutile
// Sistem responsiv - display, websocket, control PWM în timp real
// Acum poți să te concentrezi pe feature-urile principale:
// Control PWM precis
// Comunicare Modbus stabilă
// Interfață web fluentă
// OLED on pins GPIO48(SDA) and GPIO47 (SCL), +3.3V
// conectare web WiFi la 192.168.1.177 (WiFi AP întotdeauna pornit)
// ethernet webpage 192.168.1.20 (când cablul este conectat)
// touch pin 4
// Nou ! IRQ pentru ADE working
// OTA admin orbital123

#include "globals.h"
#include <dummy.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <SPI.h>
#include "ADE7758.h"
#include "ETHClass.h"
#include <WiFiClient.h>
#include <esp_sleep.h>
#include "WebServer.h"
#include "esp32-hal-cpu.h"
#include "NVRAM.h"
#include <Adafruit_MCP4725.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <esp_ota_ops.h>
#include "device_lock.h"

static bool fwConfirmed = false;

// Profiler
struct Prof {
  uint32_t lastPrintMs = 0;
  uint32_t maxWebUs = 0;
  uint32_t maxMeterUs = 0;
} prof;

static inline void profUpdate(uint32_t &m, uint32_t dt) {
  if (dt > m) m = dt;
}

static const char* otaStateName(esp_ota_img_states_t s) {
  switch(s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "?";
  }
}

void printOtaState() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  esp_err_t e = esp_ota_get_state_partition(run, &st);
  Serial.printf("RUN partition: %s, state=%s, err=%s\n",
                run ? run->label : "NULL",
                otaStateName(st),
                esp_err_to_name(e));
}

// Function declarations
void modbusTickKAandWatt();
static bool modbusWrite16(uint16_t reg, uint16_t value);
static bool modbusReadInput16(uint16_t reg, uint16_t &outValue);
static void modbusKeepAlive();
static inline uint16_t clamp16(uint32_t x) { return x > 0xFFFF ? 0xFFFF : (uint16_t)x; }
void factoryResetCalibration();

// Loop monitoring
static uint32_t g_lastLoopTime = 0;
static uint32_t g_loopCounter = 0;

// Variabile externe
extern ETHClass ETH;
extern WiFiClient client;
Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);
Adafruit_MCP4725 dac;

// SPI pentru W5500
#define SPI_FREQ 250000
#define W5500_MISO 12
#define W5500_MOSI 11
#define W5500_SCLK 13
#define W5500_CS 14
#define W5500_RST 9
#define W5500_IRQ 10

#undef MODBUS_UNIT_ID
#define MODBUS_UNIT_ID 1

// Network settings
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 20);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// Flags
bool wifiAPStarted = false;
bool lastEthernetState = true;
bool lastModbusState = true;
static bool eth_initialized = false;
unsigned long lastModbusRetry = 0;

// Modbus config
static const uint16_t MODBUS_INPUT_POTENZA_RETE = 13;
// adaugă:
static const uint16_t MODBUS_INPUT_COSFI        = 12;
static const unsigned long MODBUS_REPLY_TIMEOUT_MS = 1000;

const int MODBUS_WATT_PERIOD = 500;
const int MODBUS_KA_PERIOD = 1000;

int stay_alive = 0;
unsigned long lastStayAliveUpdate = 0;
const unsigned long MODBUS_UPDATE_INTERVAL = 1000;
unsigned long lastModbusUpdate = 0;

#define NVRAM_WP_PIN  39
const int IRQ_PIN = 15;

static SemaphoreHandle_t xAdeIrqSem = NULL;

void IRAM_ATTR adeIrqISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xAdeIrqSem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ==================== TOUCH DISPLAY CONTROL ====================
#define TOUCH_PIN 4
#define DISPLAY_TIMEOUT 60000
volatile bool displayOn = true;
volatile unsigned long lastTouchTime = 0;
volatile uint32_t g_touchBaseline = 0;   // ← adaugă asta
// ===============================================================

// Global measurement variables
double v = 0, v_a = 0, v_b = 0, v_c = 0;
double c = 0, c_a = 0, c_b = 0, c_c = 0;
extern float Watt;
float Pf = 0;
double Frequency;
float batteryVoltage = 0.0;
const int numReadings = 10;
float TensAlimentare = 0.0;
bool resetEnergyRequested = false;

static inline int32_t signExtend24(uint32_t x) {
  x &= 0xFFFFFF;
  if (x & 0x800000) x |= 0xFF000000;
  return (int32_t)x;
}

static inline int32_t deltaSigned24(uint32_t now24, uint32_t prev24) {
  uint32_t d = (now24 - prev24) & 0xFFFFFF;
  return signExtend24(d);
}

static uint32_t prevWhr24[3] = {0,0,0};
static uint32_t prevVahr24[3] = {0,0,0};
static bool pfInit = false;
static float pfFilt[3] = {1.0f,1.0f,1.0f};

// Variabile pentru debugging OTA
// La începutul fișierului .ino, după celelalte variabile externe
extern uint32_t debug_whrA;
extern uint32_t debug_whrA_prev;
extern uint32_t debug_vahrA;
extern uint32_t debug_vahrA_prev;
extern int32_t debug_dW;
extern int32_t debug_dVA;
extern float debug_pfInstant;

extern uint32_t debug_whrB;
extern uint32_t debug_whrB_prev;
extern uint32_t debug_vahrB;
extern uint32_t debug_vahrB_prev;
extern int32_t debug_dW_B;
extern int32_t debug_dVA_B;
extern float debug_pfInstant_B;

extern uint32_t debug_whrC;
extern uint32_t debug_whrC_prev;
extern uint32_t debug_vahrC;
extern uint32_t debug_vahrC_prev;
extern int32_t debug_dW_C;
extern int32_t debug_dVA_C;
extern float debug_pfInstant_C;

// Variabile rezistente
extern bool Step2;
extern bool Step3;
extern bool Step4;

#define DIV 2

// Intervale
const uint32_t MEASUREMENT_INTERVAL = 100;
const uint32_t DISPLAY_UPDATE_INTERVAL = 200;
const uint32_t ANALOG_CHECK_INTERVAL = 5000;
const uint32_t ETH_POLL_INTERVAL = 1000;
const uint32_t DEBUG_PRINT_INTERVAL = 2000;

//#define WATT_CAL 0.03961
#define WATT_CAL  16.65f
#define VA_CAL    16.27f
#define VAR_CAL  (WATT_CAL * 1.0054f)  // 14.87f
#define VRMS_CAL 15
#define IRMS_CAL 15

// PWM
const int PWM_PIN = 21;
const int PWM_CHANNEL = 0;
const int PWM_FREQ = 3000;
const int PWM_RES = 12;

int pwm;
extern int Step1;
float PowerRez = 750.0;
int numSteps = 4;
float Prez1 = 300;
float Prez2 = 125;
float Prez3 = 250;
float Prez4 = 250;
int current_step = 1;
float current_threshold = Prez1;
bool reset_pwm = true;
float pwm_accumulated = 0;

extern void sendLiveData();

TaskHandle_t TaskHandle_Core2;
TaskHandle_t WebTaskHandle;

void WebTask(void *pv) {
  initWebServerAndSocket();
  for (;;) {
    handleWebClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

float Watt_a, WattA, Watt_b, WattB, Watt_c, WattC, kWh;
float Var_a, Var_b, Var_c, Var;
float VA, VA_a, VA_b, VA_c;
float Pfa, PfA, PfB, PfC;
float voltageFactor = 1;
float currentFactor = 1;

float totalEnergy = 0.0;
extern int setpoint;
float KWh = 0.0;
unsigned long previousMillis = 0;

void factoryResetCalibration() {
  factoryCalDone = false;
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_A, 0.0f);
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_B, 0.0f);
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_C, 0.0f);
  nvramWriteFloat(ADDR_CURRENT_OFFSET_A, 0.0f);
  nvramWriteFloat(ADDR_CURRENT_OFFSET_B, 0.0f);
  nvramWriteFloat(ADDR_CURRENT_OFFSET_C, 0.0f);
  nvramWriteInt(ADDR_FACTORY_CAL_DONE, 0);
  DBG_PRINTLN("✅ Factory calibration cleared");
}

// =================== MODBUS WRITE (FC06) ===================
static bool modbusWrite16(uint16_t reg, uint16_t value) {
  if (!ETH.linkUp() || ETH.localIP() == IPAddress(0,0,0,0)) return false;
  if (!client.connected()) return false;

  if (g_modbusMutex && xSemaphoreTake(g_modbusMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  const uint16_t addr0 = MODBUS_ONE_BASED ? (reg - 1) : reg;
  const uint16_t tid = g_modbusTid++;

  uint8_t req[12] = {
    (uint8_t)(tid>>8),(uint8_t)tid, 0,0, 0,6,
    MODBUS_UNIT_ID, 0x06,
    (uint8_t)(addr0>>8),(uint8_t)(addr0&0xFF),
    (uint8_t)(value>>8),(uint8_t)(value&0xFF)
  };

  client.setTimeout(1000);
  int wrote = client.write(req, sizeof(req));
  bool success = (wrote == (int)sizeof(req));

  unsigned long startDrain = millis();
  while (client.available() > 0 && (millis() - startDrain < 500)) {
    client.read();
  }

  if (g_modbusMutex) xSemaphoreGive(g_modbusMutex);

  if (!success) {
    DBG_PRINTLN("⚠️ Modbus write failed");
    client.stop();
  }

  return success;
}

// =================== MODBUS READ INPUT (FC04) ===================
static bool modbusReadInput16(uint16_t reg, uint16_t &outValue)
{
    if (!ethernetConnected) return false;
    if (!client.connected()) return false;

    if (g_modbusMutex &&
        xSemaphoreTake(g_modbusMutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;

    bool success = false;

    do
    {
        const uint16_t addr0 = MODBUS_ONE_BASED ? (reg - 1) : reg;
        const uint16_t tid   = g_modbusTid++;

        uint8_t req[12] = {
            (uint8_t)(tid >> 8), (uint8_t)tid,
            0x00, 0x00,                 // Protocol ID
            0x00, 0x06,                 // Length
            MODBUS_UNIT_ID,
            0x04,                       // Function
            (uint8_t)(addr0 >> 8),
            (uint8_t)(addr0 & 0xFF),
            0x00, 0x01                  // 1 register
        };

        client.setTimeout(1000);

        if (client.write(req, sizeof(req)) != sizeof(req))
            break;

        // ===== Read MBAP =====
        uint8_t mbap[7];
        if (client.readBytes(mbap, 7) != 7)
            break;

        uint16_t respTid =
            ((uint16_t)mbap[0] << 8) | mbap[1];

        uint16_t protocolId =
            ((uint16_t)mbap[2] << 8) | mbap[3];

        uint16_t length =
            ((uint16_t)mbap[4] << 8) | mbap[5];

        uint8_t unitId = mbap[6];

        if (respTid != tid) break;
        if (protocolId != 0x0000) break;
        if (unitId != MODBUS_UNIT_ID) break;
        if (length != 3) break;   // 1 byte func + 1 byte count + 2 data = 4? NU.
                                  // length exclude unitId → pentru 1 reg = 3

        // ===== Read PDU =====
        uint8_t pdu[4];
        if (client.readBytes(pdu, length - 1) != (length - 1))
            break;

        uint8_t function = pdu[0];
        uint8_t byteCount = pdu[1];

        if (function & 0x80) break;   // exception
        if (function != 0x04) break;
        if (byteCount != 2) break;

        outValue = ((uint16_t)pdu[2] << 8) | pdu[3];

        success = true;

    } while (false);

    if (!success)
    {
        while (client.available()) client.read();
    }

    if (g_modbusMutex) xSemaphoreGive(g_modbusMutex);

    return success;
}

// =================== CONTROL PWM (STABIL + FARA SOC) ===================
void updatePWMFromWatt(void *pvParameters)
{
    static int pwmFiltered = 0;
    static bool prev_s2 = false, prev_s3 = false, prev_s4 = false;

    const int PWM_STEP = 400;   // 120 rampă rapidă dar fără șoc

    while (1)
    {
        if (!manualControlEnabled)
        {
            float wattIn = WattA + WattB + WattC;
          float diffW = wattIn - (float)setpoint;

            float p1 = prez1;
            float p2 = prez2;
            float p3 = prez3;
            float p4 = prez4;

            bool s2 = false;
            bool s3 = false;
            bool s4 = false;

            float digital = 0.0f;

            // ===== LOGICA TA ORIGINALĂ (CORECTĂ MATEMATIC) =====
            if (diffW > 0)
            {
                if (p2 > 0 && diffW >= p2)
                {
                    s2 = true;
                    digital += p2;
                }

                if (p3 > 0 && diffW >= digital + p3)
                {
                    s3 = true;
                    digital += p3;
                }

                if (p4 > 0 && diffW >= digital + p4)
                {
                    s4 = true;
                    digital += p4;
                }
            }

            // ===== ANALOG =====
            float pwmW = diffW - digital;

            if (pwmW < 0) pwmW = 0;
            if (pwmW > p1) pwmW = p1;

            int pwmTarget = (p1 > 0) ? (int)((pwmW / p1) * 4095.0f) : 0;

            // ===== FEED-FORWARD la schimbarea stepurilor =====
            if (s2 != prev_s2) {
                int ff = (int)((p2 / p1) * 4095.0f);
                pwmFiltered += s2 ? -ff : ff;
                pwmFiltered = constrain(pwmFiltered, 0, 4095);
                prev_s2 = s2;
            }
            if (s3 != prev_s3) {
                int ff = (int)((p3 / p1) * 4095.0f);
                pwmFiltered += s3 ? -ff : ff;
                pwmFiltered = constrain(pwmFiltered, 0, 4095);
                prev_s3 = s3;
            }
            if (s4 != prev_s4) {
                int ff = (int)((p4 / p1) * 4095.0f);
                pwmFiltered += s4 ? -ff : ff;
                pwmFiltered = constrain(pwmFiltered, 0, 4095);
                prev_s4 = s4;
            }

            // ===== RAMPĂ DOAR PE PWM =====
            int delta = pwmTarget - pwmFiltered;

            if (delta > PWM_STEP) delta = PWM_STEP;
            if (delta < -PWM_STEP) delta = -PWM_STEP;

            pwmFiltered += delta;

            pwm1 = pwmFiltered;
            Step1 = pwm1;

            Step2 = s2;
            Step3 = s3;
            Step4 = s4;

        }

        ledcWrite(0, pwm1);
        dac.setVoltage((uint16_t)pwm1, false);

        digitalWrite(STEP2_PIN, Step2);
        digitalWrite(STEP3_PIN, Step3);
        digitalWrite(STEP4_PIN, Step4);

       

        vTaskDelay(pdMS_TO_TICKS(20));
 
    }
}

void setPWM(int val) {
  int invertedVal = 4095 - val;
  ledcWrite(PWM_CHANNEL, invertedVal);
}

// =================== WiFi/Ethernet Events ===================
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      DBG_PRINTLN("Ethernet started");
      ETH.setHostname("ESP32-Modbus");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      DBG_PRINTLN("Ethernet connected");
      DBG_PRINT("IP Address: ");
      DBG_PRINTLN(ETH.localIP());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      DBG_PRINTLN("Ethernet disconnected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      DBG_PRINT("Ethernet IP: ");
      DBG_PRINTLN(ETH.localIP());
      break;
    default:
      break;
  }
}

bool safeGPIOInit() {
  DBG_PRINTLN("Starting safe GPIO initialization...");

  Step1 = 0;
  Step2 = false;
  Step3 = false;
  Step4 = false;
  pwm1 = 0;

  pinMode(STEP2_PIN, INPUT_PULLUP);
  pinMode(STEP3_PIN, INPUT_PULLUP);
  pinMode(STEP4_PIN, INPUT_PULLUP);
  delay(10);

  bool success = true;

  pinMode(STEP2_PIN, OUTPUT);
  digitalWrite(STEP2_PIN, LOW);
  delay(100);

  pinMode(STEP3_PIN, OUTPUT);
  digitalWrite(STEP3_PIN, LOW);
  delay(100);

  pinMode(STEP4_PIN, OUTPUT);
  digitalWrite(STEP4_PIN, LOW);
  delay(100);

  DBG_PRINTLN("✅ GPIO initialization complete");
  return success;
}

void resetW5500() {
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(500);
  digitalWrite(W5500_RST, HIGH);
  delay(500);
  DBG_PRINTLN("🔄 W5500 reset complet!");
}

// =================== TOUCH HANDLERS ===================
void checkTouchTimeout() {
  static bool wasTouched = false;
  uint32_t touchValue = touchRead(TOUCH_PIN);

  // Prag relativ: atingere = valoare cu 50% mai mare decât baseline
  uint32_t threshold_high = g_touchBaseline + (g_touchBaseline / 2);
  uint32_t threshold_low  = g_touchBaseline + (g_touchBaseline / 4);

  bool isTouched;
  if (!wasTouched) {
    isTouched = (touchValue > threshold_high);
  } else {
    isTouched = (touchValue > threshold_low);
  }

  if (isTouched && !wasTouched) {
    lastTouchTime = millis();
    if (!displayOn) {
      displayOn = true;
      DBG_PRINTLN("🔆 Display ON");
    }
  }
  wasTouched = isTouched;

  if (displayOn && (millis() - lastTouchTime > DISPLAY_TIMEOUT)) {
    displayOn = false;
    DBG_PRINTLN("🌙 Display OFF");
  }
}


void OledTask(void *pvParameters) {
  for (;;) {
    checkTouchTimeout();

    static bool cleared = false;

    if (displayOn) {
      cleared = false;

      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        display.clearDisplay();

        display.setTextSize(2);
        display.setCursor(2, 0);
        display.print(Watt / 1000.0f); // afiseaza in kW
        display.setCursor(100, 0);
        display.print("kW");

        display.setCursor(2, 16);
        display.print(v_a);
        display.setCursor(100,16);
        display.print("V");

        display.setCursor(2, 32);
        display.print(c_a);
        display.setCursor(100,32);
        display.print("A");

        display.setTextSize(1);
        display.setCursor(2, 48);
        display.print("PWM: ");
        display.setCursor(28, 48);
        display.print(pwm1);
        display.setCursor(68,48);
        display.print("Pf: ");
        display.setCursor(90, 48);
        display.print(PfA);

        display.setCursor(24,57);
        display.print("kWh:  ");
        display.setCursor(49, 57);
        display.print(KWh);

        display.setCursor(0, 57);
        if (ETH.linkUp() && modbusConnected && !g_netBusy) {
          display.print("MB");
        } else if (ETH.linkUp()) {
          display.print("Eth");
        } else {
          display.print("--");
        }

        display.display();
        xSemaphoreGive(i2cMutex);
      }
    } else {
      if (!cleared && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        display.clearDisplay();
        display.display();
        xSemaphoreGive(i2cMutex);
        cleared = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ==================== Task Modbus ====================
void ModbusTask(void *pvParameters) {
  static uint16_t ka = 0;
  static uint32_t lastReconnectAttempt = 0;
  static uint32_t lastReadPwrRete = 0;
  static uint32_t lastReadCosfi = 0;

  for (;;) {
    if (ethernetConnected && modbusConnected) {
      if (!client.connected()) {
        DBG_PRINTLN("⚠️ Modbus dead → reconnecting");
        client.stop();
        modbusConnected = false;
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      bool wattSuccess = false;
      uint16_t watt_int = (uint16_t)((uint32_t)(Watt / 100.0f) & 0xFFFF);
      for (int retry = 0; retry < 3; retry++) {
        if (modbusWrite16(MODBUS_REG_WATT, watt_int)) {
          wattSuccess = true;
          g_lastModbusTxMs = millis();
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (!wattSuccess) {
        DBG_PRINTLN("❌ WATT failed → reconnecting");
        client.stop();
        modbusConnected = false;
        continue;
      }

      bool kaSuccess = false;
      for (int retry = 0; retry < 3; retry++) {
        if (modbusWrite16(MODBUS_REG_STAY_ALIVE, ka++)) {
          kaSuccess = true;
          g_lastModbusTxMs = millis();
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (!kaSuccess) {
        DBG_PRINTLN("❌ KA failed → reconnecting");
        client.stop();
        modbusConnected = false;
        continue;
      }

      if (ka > 9999) ka = 1;

      const uint32_t now = millis();
      if (now - lastReadPwrRete >= 1000) {
        uint16_t raw = 0;
        bool ok = false;
        for (int retry = 0; retry < 2; retry++) {
          if (modbusReadInput16(MODBUS_INPUT_POTENZA_RETE, raw)) {
            ok = true;
            g_lastModbusRxMs = millis();
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(80));
        }
        if (ok) {
          int16_t s = (int16_t)raw;
          PowerRete = (float)s / 10.0f;
        }
        lastReadPwrRete = now;

        if (now - lastReadCosfi >= 1000) {
        uint16_t rawCosfi = 0;
        bool okCosfi = false;
        for (int retry = 0; retry < 2; retry++) {
          if (modbusReadInput16(MODBUS_INPUT_COSFI, rawCosfi)) {
            okCosfi = true;
            g_lastModbusRxMs = millis();
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(80));
        }
        if (okCosfi) {
          int16_t sCosfi = (int16_t)rawCosfi;
          float cf = (float)sCosfi / 100.0f;
          CosfiRete = constrain(cf, -1.0f, 1.0f);
          DBG_PRINTf("📡 CosfiRete = %.3f (raw=%d)\n", CosfiRete, sCosfi);
        }
        lastReadCosfi = now;
      }
      }

    } else if (ethernetConnected && !modbusConnected) {
      if (millis() - lastReconnectAttempt >= 5000 || lastReconnectAttempt == 0) {
        DBG_PRINT("🔗 Modbus reconnect to ");
        DBG_PRINTLN(MODBUS_SLAVE_IP);

        client.stop();

        uint32_t startAttempt = millis();
        bool connected = false;
        while (millis() - startAttempt < 3000) {
          if (client.connect(MODBUS_SLAVE_IP, MODBUS_PORT)) {
            connected = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (connected) {
          DBG_PRINTLN("🎉 Modbus connected!");
          modbusConnected = true;
          client.setTimeout(2000);
          g_lastModbusTxMs = millis();
          lastReconnectAttempt = millis();
        } else {
          DBG_PRINTLN("❌ Modbus reconnect failed");
          lastReconnectAttempt = millis();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void modbusTickKAandWatt() {
  static uint32_t lastWattSend = 0;
  static uint32_t lastKASend = 0;
  const uint32_t now = millis();

  if (!ethernetConnected) {
    modbusConnected = false;
    return;
  }

  if (!modbusConnected) {
    return;
  }

  if (now - lastWattSend >= 500) {
    uint32_t wattInt = (uint32_t)(Watt / 100.0f);
    if (wattInt > 0xFFFF) wattInt = 0xFFFF;

    if (!modbusWrite16(MODBUS_REG_WATT, (uint16_t)wattInt)) {
      DBG_PRINTLN("❌ WATT write failed");
      client.stop();
      modbusConnected = false;
      return;
    }
    lastWattSend = now;
  }

  if (now - lastKASend >= 1000) {
    static uint16_t kaValue = 1;
    if (!modbusWrite16(MODBUS_REG_STAY_ALIVE, kaValue)) {
      DBG_PRINTLN("❌ KA write failed");
      client.stop();
      modbusConnected = false;
      return;
    }
    kaValue++;
    if (kaValue > 9999) kaValue = 1;
    lastKASend = now;
  }
}

// ═══════════════════════════════════════════════════════════
// 🔧 CALIBRARE AUTO-ZERO
// ═══════════════════════════════════════════════════════════
void calibrazioneAutomaticaOffset() {
  Serial.println("\n╔═══════════════════════════════════════════════════════════╗");
  Serial.println("║        🔧 CALIBRARE AUTO-ZERO                            ║");
  Serial.println("╚═══════════════════════════════════════════════════════════╝");
  Serial.println();
  Serial.println("⚠️  DECONECTEAZĂ TOATE SARCINILE!");
  Serial.println();
  
  long sumI_A = 0, sumI_B = 0, sumI_C = 0;
  long sumV_A = 0, sumV_B = 0, sumV_C = 0;
  
  const int NUM_SAMPLES = 50;
  int validSamples = 0;
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    long rawI_A = meter.getIRMS(PHASE_A);
    long rawI_B = meter.getIRMS(PHASE_B);
    long rawI_C = meter.getIRMS(PHASE_C);
    
    long rawV_A = meter.getVRMS(PHASE_A);
    long rawV_B = meter.getVRMS(PHASE_B);
    long rawV_C = meter.getVRMS(PHASE_C);
    
    if (rawI_A != 0xFFFFFF && rawI_B != 0xFFFFFF && rawI_C != 0xFFFFFF &&
        rawV_A != 0xFFFFFF && rawV_B != 0xFFFFFF && rawV_C != 0xFFFFFF) {
      
      sumI_A += rawI_A;
      sumI_B += rawI_B;
      sumI_C += rawI_C;
      
      sumV_A += rawV_A;
      sumV_B += rawV_B;
      sumV_C += rawV_C;
      
      validSamples++;
    }
    
    if ((i + 1) % 10 == 0) {
      Serial.printf("  Progress: %d/%d\n", i + 1, NUM_SAMPLES);
    }
    
    delay(50);
  }
  
  if (validSamples < NUM_SAMPLES * 0.8) {
    Serial.printf("❌ EROARE: Doar %d/%d eșantioane valide!\n", validSamples, NUM_SAMPLES);
    return;
  }
  
  currentOffsetA = (float)sumI_A / (float)validSamples;
  currentOffsetB = (float)sumI_B / (float)validSamples;
  currentOffsetC = (float)sumI_C / (float)validSamples;
  
  voltageOffsetA = (float)sumV_A / (float)validSamples;
  voltageOffsetB = (float)sumV_B / (float)validSamples;
  voltageOffsetC = (float)sumV_C / (float)validSamples;
  
  Serial.println("\n💾 Salvare în NVRAM...");
  
  nvramWriteFloat(ADDR_CURRENT_OFFSET_A, currentOffsetA);
  nvramWriteFloat(ADDR_CURRENT_OFFSET_B, currentOffsetB);
  nvramWriteFloat(ADDR_CURRENT_OFFSET_C, currentOffsetC);
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_A, voltageOffsetA);
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_B, voltageOffsetB);
  nvramWriteFloat(ADDR_VOLTAGE_OFFSET_C, voltageOffsetC);
  
  Serial.println("\n✅ CALIBRARE COMPLETĂ!");
  Serial.printf("Offset I: A=%.0f B=%.0f C=%.0f\n", currentOffsetA, currentOffsetB, currentOffsetC);
  Serial.printf("Offset V: A=%.0f B=%.0f C=%.0f\n", voltageOffsetA, voltageOffsetB, voltageOffsetC);
  
  factoryCalDone = true;
  nvramWriteInt(ADDR_FACTORY_CAL_DONE, 1);
}


// ═══════════════════════════════════════════════════════════
// 📊 FUNCȚIA PRINCIPALĂ DE MĂSURARE - CU PF DIN PUTERE INSTANTANEE
// ═══════════════════════════════════════════════════════════
void doMeasurements() {
  const uint32_t now_ms = millis();
  static uint32_t lastMeasurement = 0;
  
  // LINECYC=30, 3 faze ZXSEL → 30 ZX / (3×2×50Hz) = 100ms acumulare
  // Așteptăm 120ms pentru siguranță
  if (now_ms - lastMeasurement < 120) return;
  lastMeasurement = now_ms;

  uint32_t tMeter = micros();
  // Citire frecvență
  uint16_t freq_raw = meter.getFreq() & 0x0FFF;
  Frequency = (double)freq_raw / (double)FREQ_CAL;
  
  // ========== CITIRI SINCRONIZATE VRMS/IRMS ==========
  uint32_t vrmsA = meter.getVRMS_raw(PHASE_A);
  uint32_t vrmsB = meter.getVRMS_raw(PHASE_B);
  uint32_t vrmsC = meter.getVRMS_raw(PHASE_C);

  uint32_t irmsA = meter.getIRMS_raw(PHASE_A);
  uint32_t irmsB = meter.getIRMS_raw(PHASE_B);
  uint32_t irmsC = meter.getIRMS_raw(PHASE_C);

  // ========== CITIRI SINCRONIZATE AWATTHR/AVAHR — O SINGURĂ DATĂ ==========
  int32_t rawWattA = meter.getWatt(PHASE_A);
  int32_t rawWattB = meter.getWatt(PHASE_B);
  int32_t rawWattC = meter.getWatt(PHASE_C);

  int32_t rawVaA = meter.getVA(PHASE_A);
  int32_t rawVaB = meter.getVA(PHASE_B);
  int32_t rawVaC = meter.getVA(PHASE_C);

  // ========== PROCESARE VRMS/IRMS ==========
  float CT_Ratio = (float)CT_Primary / (float)CT_Secondary;  // universala
  const float VOLTAGE_THRESHOLD = 10.0f;
  const float CURRENT_THRESHOLD = 0.15f;
  
  float vA_counts = fmaxf(0.0f, (float)vrmsA - voltageOffsetA);
  float vB_counts = fmaxf(0.0f, (float)vrmsB - voltageOffsetB);
  float vC_counts = fmaxf(0.0f, (float)vrmsC - voltageOffsetC);
  
  float v_a_raw = vA_counts * VRMS_SCALE_A * voltageFactor;
  float v_b_raw = vB_counts * VRMS_SCALE_B * voltageFactor;
  float v_c_raw = vC_counts * VRMS_SCALE_C * voltageFactor;
  
  float iA_counts = fmaxf(0.0f, (float)irmsA - currentOffsetA);
  float iB_counts = fmaxf(0.0f, (float)irmsB - currentOffsetB);
  float iC_counts = fmaxf(0.0f, (float)irmsC - currentOffsetC);
  
float c_a_raw = iA_counts * IRMS_SCALE_BASE_A * CT_Ratio * currentFactor;
float c_b_raw = iB_counts * IRMS_SCALE_BASE_B * CT_Ratio * currentFactor;
float c_c_raw = iC_counts * IRMS_SCALE_BASE_C * CT_Ratio * currentFactor;
  
  v_a = (v_a_raw > VOLTAGE_THRESHOLD) ? v_a_raw : 0.0f;
  v_b = (v_b_raw > VOLTAGE_THRESHOLD) ? v_b_raw : 0.0f;
  v_c = (v_c_raw > VOLTAGE_THRESHOLD) ? v_c_raw : 0.0f;
  
  c_a = (c_a_raw > CURRENT_THRESHOLD) ? c_a_raw : 0.0f;
  c_b = (c_b_raw > CURRENT_THRESHOLD) ? c_b_raw : 0.0f;
  c_c = (c_c_raw > CURRENT_THRESHOLD) ? c_c_raw : 0.0f;

  // ========== GATING: faza activă? ==========
  bool activeA = (v_a > VOLTAGE_THRESHOLD && c_a > CURRENT_THRESHOLD);
  bool activeB = (v_b > VOLTAGE_THRESHOLD && c_b > CURRENT_THRESHOLD);
  bool activeC = (v_c > VOLTAGE_THRESHOLD && c_c > CURRENT_THRESHOLD);

  // ========== PUTERI DIN ADE (scalate, o singură dată) ==========
  float wA = activeA ? (float)rawWattA * WATT_CAL : 0.0f;
  float wB = activeB ? (float)rawWattB * WATT_CAL : 0.0f;
  float wC = activeC ? (float)rawWattC * WATT_CAL : 0.0f;

  float vaA = activeA ? (float)rawVaA * VA_CAL : 0.0f;
  float vaB = activeB ? (float)rawVaB * VA_CAL : 0.0f;
  float vaC = activeC ? (float)rawVaC * VA_CAL : 0.0f;

  // ========== SELECTIE SURSA COS φ ==========
  bool modbusOk = ethernetConnected && modbusConnected;
  bool useRete  = modbusOk && pfUseRete;  // Modbus disponibil ȘI utilizatorul a ales Rete

  if (useRete) {
    // Sursa: Modbus (CosfiRete citit din rețea)
    VA_a = activeA ? v_a * c_a : 0.0f;
    VA_b = activeB ? v_b * c_b : 0.0f;
    VA_c = activeC ? v_c * c_c : 0.0f;

    PfA = (VA_a > 0.1f) ? constrain(CosfiRete, -1.0f, 1.0f) : 0.0f;
    PfB = (VA_b > 0.1f) ? constrain(CosfiRete, -1.0f, 1.0f) : 0.0f;
    PfC = (VA_c > 0.1f) ? constrain(CosfiRete, -1.0f, 1.0f) : 0.0f;

    WattA = VA_a * PfA;
    WattB = VA_b * PfB;
    WattC = VA_c * PfC;

  } else {
    // Sursa: ADE7758 calculat intern
    VA_a = vaA;
    VA_b = vaB;
    VA_c = vaC;

    WattA = wA;
    WattB = wB;
    WattC = wC;

    PfA = (vaA > 0.1f && activeA) ? constrain(wA / vaA, -1.0f, 1.0f) : 0.0f;
    PfB = (vaB > 0.1f && activeB) ? constrain(wB / vaB, -1.0f, 1.0f) : 0.0f;
    PfC = (vaC > 0.1f && activeC) ? constrain(wC / vaC, -1.0f, 1.0f) : 0.0f;
  }

  VA   = VA_a + VA_b + VA_c;
  Watt = WattA + WattB + WattC;

  // ========== PUTERI REACTIVE ==========
  float Qa = sqrtf(fmaxf(0.0f, VA_a*VA_a - WattA*WattA));
  Var_a = (PfA < 0.0f) ? -Qa : Qa;

  float Qb = sqrtf(fmaxf(0.0f, VA_b*VA_b - WattB*WattB));
  Var_b = (PfB < 0.0f) ? -Qb : Qb;

  float Qc = sqrtf(fmaxf(0.0f, VA_c*VA_c - WattC*WattC));
  Var_c = (PfC < 0.0f) ? -Qc : Qc;

  Var = Var_a + Var_b + Var_c;

  // ========== PF TOTAL ==========
  float sumVA = VA_a + VA_b + VA_c;
  Pf = (sumVA > 0.1f)
       ? (VA_a*PfA + VA_b*PfB + VA_c*PfC) / sumVA
       : 1.0f;

  // ========== DEBUG (la 5s) ==========
  static uint32_t lastDebugPrint = 0;
  if (now_ms - lastDebugPrint > 5000) {
    lastDebugPrint = now_ms;
    Serial.printf(
      "[MEAS] sursa=%s CosfiRete=%.3f | Pf: A=%.2f B=%.2f C=%.2f | "
      "VA: A=%.2f B=%.2f C=%.2f | W: A=%.2f B=%.2f C=%.2f | Watt=%.2f W\n",
      modbusOk && pfUseRete ? "Modbus" : "ADE7758",
      CosfiRete,
      PfA, PfB, PfC,
      VA_a, VA_b, VA_c,
      WattA, WattB, WattC, Watt
    );
    Serial.printf(
      "[ADE]  rawW: A=%ld B=%ld C=%ld | rawVA: A=%ld B=%ld C=%ld | "
      "wA=%.2f vaA=%.2f W/VA=%.4f\n",
      rawWattA, rawWattB, rawWattC,
      rawVaA, rawVaB, rawVaC,
      wA, vaA, (vaA > 0.001f) ? wA/vaA : 0.0f
    );
  }

  profUpdate(prof.maxMeterUs, micros() - tMeter);
}

// ========== TASK PENTRU MĂSURĂRI ==========
void TaskMeter(void *pv) {
  for (;;) {
    if (xSemaphoreTake(xAdeIrqSem, pdMS_TO_TICKS(200)) == pdTRUE) {
      meter.resetStatus();
      doMeasurements();
    } else {
      Serial.println("⚠️ ADE7758 IRQ timeout - citire forțată");
      meter.resetStatus();
      doMeasurements();
    }
  }
}

// ========== FUNCȚIA PENTRU RESET REASON ==========
const char* resetReasonToStr(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:    return "Unknown";
    case ESP_RST_POWERON:    return "Power On";
    case ESP_RST_EXT:        return "External Reset";
    case ESP_RST_SW:         return "Software Reset";
    case ESP_RST_PANIC:      return "Panic";
    case ESP_RST_INT_WDT:    return "Interrupt Watchdog";
    case ESP_RST_TASK_WDT:   return "Task Watchdog";
    case ESP_RST_WDT:        return "Other Watchdog";
    case ESP_RST_DEEPSLEEP:  return "Deep Sleep";
    case ESP_RST_BROWNOUT:   return "Brownout";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "Other";
  }
}

// ⬅️ AICI SE TERMINĂ CODUL - URMEAZĂ FUNCȚIA setup()

void setup() {
  // ═══════════════════════════════════════════════════════════
  // 🔧 INIȚIALIZARE GPIO (PRIMA ACȚIUNE ABSOLUTĂ!)
  // ═══════════════════════════════════════════════════════════
    Serial.begin(115200);
  delay(1000);


  Serial.println("\n\n");
  Serial.println("═══════════════════════════════════════════");
  Serial.println("  🚀 FIRMWARE VERSION: v9.0  OTA");  // ← SCHIMBĂ VERSIUNEA
  Serial.println("═══════════════════════════════════════════");
  Serial.println();

  Serial.println("BOOT: entered setup()");
  safeGPIOInit();
  delay(10);
  
  // 🔹 Inițializare pin pentru control alimentare stepuri (mosfet P)
  pinMode(STEPS_POWER_EN_PIN, OUTPUT);
  digitalWrite(STEPS_POWER_EN_PIN, stepsEnabled ? LOW : HIGH);  // LOW = ON, HIGH = OFF
  Serial.printf("🔌 Pin alimentare stepuri (GPIO %d): %s\n", 
                STEPS_POWER_EN_PIN, stepsEnabled ? "ON" : "OFF");
  
  // ═══════════════════════════════════════════════════════════
  // ⚡ CONFIGURARE PWM
  // ═══════════════════════════════════════════════════════════
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcWrite(PWM_CHANNEL, 0);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
  setPWM(0);



  // ═══════════════════════════════════════════════════════════
  // 📡 PORNIRE SERIAL
  // ═══════════════════════════════════════════════════════════
 // Serial.begin(115200);
  //delay(2000);  // Așteaptă stabilizarea Serial
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════════════════╗");
  Serial.println("║                ESP32-S3 POWER MONITOR V09                 ║");
  Serial.println("║              OTA Rollback + Dual Network                  ║");
  Serial.println("╚═══════════════════════════════════════════════════════════╝");
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🔍 VERIFICARE COMPLETĂ CONFIGURAȚIE OTA
  // ═══════════════════════════════════════════════════════════
  
  Serial.println("┌───────────────────────────────────────┐");
  Serial.println("│    OTA Configuration Check            │");
  Serial.println("└───────────────────────────────────────┘");
  
  // 1️⃣ Verifică dacă rollback este activat în build
  bool rollbackEnabled = false;
  #ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    Serial.println("✅ Rollback: ENABLED");
    rollbackEnabled = true;
  #else
    Serial.println("❌ Rollback: DISABLED");
    Serial.println();
    Serial.println("⚠️  ATENȚIE: Rollback-ul nu este activat!");
    Serial.println("📋 Soluție:");
    Serial.println("   1. Creează fișier 'sdkconfig' în folderul sketch-ului");
    Serial.println("   2. Adaugă: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y");
    Serial.println("   3. Închide și redeschide Arduino IDE");
    Serial.println("   4. Recompilează firmware-ul");
    Serial.println();
  #endif
  
  // 2️⃣ Verifică existența partițiilor OTA
  const esp_partition_t* ota0 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t* ota1 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  const esp_partition_t* otadata = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  
  bool partitionsOK = false;
  if (ota0 && ota1 && otadata) {
    Serial.println("✅ Partiții OTA: Configurate corect");
    Serial.printf("   • OTA_0: 0x%06X (%u KB)\n", ota0->address, ota0->size / 1024);
    Serial.printf("   • OTA_1: 0x%06X (%u KB)\n", ota1->address, ota1->size / 1024);
    partitionsOK = true;
  } else {
    Serial.println("❌ Partiții OTA: Incomplete!");
    if (!ota0) Serial.println("   ✗ Lipsește: OTA_0");
    if (!ota1) Serial.println("   ✗ Lipsește: OTA_1");
    if (!otadata) Serial.println("   ✗ Lipsește: OTADATA");
    Serial.println();
    Serial.println("📋 Soluție:");
    Serial.println("   Tools → Partition Scheme");
    Serial.println("   Alege: '16M Flash (3MB APP/9.9MB FATFS)'");
    Serial.println();
  }
  
  // 3️⃣ Afișează starea partițiilor curente
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  
  if (running) {
    Serial.printf("Running partition: %s (0x%06X)\n", running->label, running->address);
  }
  if (boot) {
    Serial.printf("Boot partition:    %s (0x%06X)\n", boot->label, boot->address);
  }
  
  // 4️⃣ Verifică starea firmware-ului
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t err = esp_ota_get_state_partition(running, &state);
  
  if (err == ESP_OK) {
    Serial.print("Firmware State:    ");
    switch(state) {
      case ESP_OTA_IMG_NEW:
        Serial.println("NEW (prima pornire după OTA)");
        break;
      case ESP_OTA_IMG_PENDING_VERIFY:
        Serial.println("PENDING_VERIFY ⏳");
        Serial.println();
        Serial.println("⚠️  Firmware va fi confirmat după 15 secunde");
        Serial.println("    SAU va face rollback dacă crash-uiește!");
        Serial.println();
        break;
      case ESP_OTA_IMG_VALID:
        Serial.println("VALID ✅");
        break;
      case ESP_OTA_IMG_INVALID:
        Serial.println("INVALID ❌");
        break;
      case ESP_OTA_IMG_ABORTED:
        Serial.println("ABORTED");
        break;
      default:
        Serial.println("UNDEFINED");
        break;
    }
  } else {
    Serial.printf("⚠️  Nu pot determina starea: %s\n", esp_err_to_name(err));
  }
  
  // 5️⃣ Sumarul configurației OTA
  Serial.println();
  if (rollbackEnabled && partitionsOK) {
    Serial.println("🎉 OTA ROLLBACK: Complet configurat și funcțional!");
  } else {
    Serial.println("⚠️  OTA ROLLBACK: NU este complet configurat!");
    if (!rollbackEnabled) Serial.println("   → Lipsește configurarea rollback");
    if (!partitionsOK) Serial.println("   → Lipsesc partiții OTA");
  }
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🔁 INFORMAȚII RESET
  // ═══════════════════════════════════════════════════════════
  auto rr = esp_reset_reason();
  Serial.printf("Reset reason: %s (%d)\n", resetReasonToStr(rr), rr);
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🔒 INIȚIALIZARE MUTEX-URI
  // ═══════════════════════════════════════════════════════════
  if (i2cMutex == nullptr) {
    i2cMutex = xSemaphoreCreateMutex();
    Serial.println("✅ I2C Mutex creat");
  }
  if (spiMutex == nullptr) {
    spiMutex = xSemaphoreCreateMutex();
    Serial.println("✅ SPI Mutex creat");
  }
  if (g_modbusMutex == nullptr) {
    g_modbusMutex = xSemaphoreCreateMutex();
    Serial.println("✅ Modbus Mutex creat");
  }
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // ⚙️  SETĂRI SISTEM
  // ═══════════════════════════════════════════════════════════
  setCpuFrequencyMhz(240);
  Serial.printf("CPU Frequency: %d MHz\n", getCpuFrequencyMhz());
  
  preferences.begin("settings", false);
  Serial.println("✅ NVRAM (Preferences) inițializat");
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 👆 TOUCH SENSOR
  // ═══════════════════════════════════════════════════════════
  displayOn = true;
  lastTouchTime = millis();
  Serial.println("✅ Touch sensor (GPIO4) configurat");
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🔄 RESET W5500 ETHERNET
  // ═══════════════════════════════════════════════════════════
  Serial.println("Resetare W5500...");
  resetW5500();
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🔌 INIȚIALIZARE I2C (OLED + DAC + EEPROM)
  // ═══════════════════════════════════════════════════════════
  Wire.begin(48, 47);  // SDA=GPIO48, SCL=GPIO47
  Wire.setTimeout(50);
  Serial.println("✅ I2C Bus inițializat (GPIO48/47)");


        // ── VERIFICARE ANTI-CLONARE ──────────────────────────────────────────
    if (!deviceLock_init()) {
        Serial.println("SISTEM BLOCAT - HARDWARE INVALID");
        
        // Optiunea 1: loop infinit
        while (true) {
            delay(10000);  // sta blocat, nu face nimic
        }
  } 
  // ═══════════════════════════════════════════════════════════
  // 📊 DAC MCP4725 - Configurare Sigură
  // ═══════════════════════════════════════════════════════════
  Serial.println("Configurare DAC MCP4725...");
  dac.begin(0x60);
  
  bool dacEepromSet = preferences.getBool("dac_eeprom", false);
  
  if (!dacEepromSet) {
    Serial.println("  → Prima pornire - salvez 0V în EEPROM DAC");
    dac.setVoltage(0, true);  // Salvează în EEPROM
    delay(50);
    preferences.putBool("dac_eeprom", true);
    Serial.println("  → ✅ EEPROM DAC configurat (0V la pornire)");
  } else {
    dac.setVoltage(0, false);  // Doar setează la 0V
    Serial.println("  → DAC setat la 0V (EEPROM deja configurat)");
  }
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🖥️  DISPLAY OLED SSD1306
  // ═══════════════════════════════════════════════════════════
  Serial.println("Inițializare OLED Display...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ OLED nu a fost detectat!");
  } else {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(32, 0);
    display.println("EBS");
    display.setTextSize(1);
    display.setCursor(2, 28);
    display.print("Booting V9.0...");
    display.display();
    Serial.println("✅ OLED Display activ");
  }
  Serial.println();

// ═══════════════════════════════════════════════════════════
// ⚡ ADE7758 ENERGY METER - CONFIGURARE DEFINITIVĂ
// ═══════════════════════════════════════════════════════════
Serial.println("⚡ Configurare ADE7758 Energy Meter...\n");

// Inițializare SPI hardware
SPI.begin(8, 6, 7, 5);
Serial.println("\n🔍 Test hardware MISO:");
pinMode(6, INPUT);
Serial.printf("MISO (GPIO6) idle = %d\n", digitalRead(6));
delay(100);
SPI.setFrequency(SPI_FREQ);

// NU apela Init() - configurăm totul manual!
pinMode(5, OUTPUT);
digitalWrite(5, HIGH);
delay(100);

// ───────────────────────────────────────────────────────────
// PASUL 1: SOFTWARE RESET COMPLET
// ───────────────────────────────────────────────────────────
Serial.println("1️⃣ Software reset complet...");
meter.write8bits(OPMODE, SWRST);
delay(50);

// ───────────────────────────────────────────────────────────
// PASUL 2: CONFIGURARE OPMODE
// ───────────────────────────────────────────────────────────
Serial.println("2️⃣ Configurare OPMODE (HPF enabled)...");
meter.write8bits(OPMODE, 0x00);
delay(20);

uint8_t opmode_check = meter.read8bits(OPMODE);
if (opmode_check != DISCF) {
  Serial.printf("  ⚠️  OPMODE greșit (0x%02X), rescriu...\n", opmode_check);
  meter.write8bits(OPMODE, 0x00);
  delay(20);
  opmode_check = meter.read8bits(OPMODE);
  Serial.printf("  → OPMODE acum: 0x%02X\n", opmode_check);
}

// ───────────────────────────────────────────────────────────
// PASUL 3: CONFIGURARE REGISTRE DE BAZĂ
// ───────────────────────────────────────────────────────────
Serial.println("3️⃣ Configurare registre de bază...");

// 0x78 = 0111 1000  → RSTREAD=1 ← PROBLEMA (resetează registrul după citire)
// 0x38 = 0011 1000  → RSTREAD=0 ← CORECT

meter.write8bits(LCYCMODE, 0x3F);  // LWATT+LVAR+LVA + ZXSEL ABC + RSTREAD=0
delay(50);

uint8_t lcyc_check = meter.read8bits(LCYCMODE);
Serial.printf("LCYCMODE verificat: 0x%02X (trebuie 0x38)\n", lcyc_check);

if (lcyc_check != 0x38) {
    Serial.println("⚠️  LCYCMODE greșit! Reconfigurez...");
    meter.write16bits(LINECYC, 30);  // 30 zero-crossings = 300ms la 50Hz
    delay(50);
}
// ACUM scrie LINECYC
meter.write16bits(LINECYC, 30);
delay(100);  // Așteaptă activarea acumulării

// Verifică LINECYC prin citire
uint16_t linecyc_val = meter.read16bits(LINECYC);
Serial.printf("LINECYC după configurare: %u (0x%04X)\n", linecyc_val, linecyc_val);

// Dacă tot nu e corect, încearcă o valoare mai simplă
if (linecyc_val != 30) {
    Serial.println("LINECYC tot greșit - încerc valoare 100...");
    meter.write16bits(LINECYC, 100);
    delay(100);
    linecyc_val = meter.read16bits(LINECYC);
    Serial.printf("LINECYC acum: %u\n", linecyc_val);
}

// ───────────────────────────────────────────────────────────
// PASUL 4: CONFIGURARE GAIN
// ───────────────────────────────────────────────────────────
Serial.println("4️⃣ Configurare GAIN registers...");

meter.write8bits(GAIN, 0x00);
delay(20);

uint8_t gain_check = meter.read8bits(GAIN);
if (gain_check != 0x00) {
  Serial.printf("  ⚠️  GAIN greșit (0x%02X), rescriu...\n", gain_check);
  meter.write8bits(GAIN, 0x00);
  delay(20);
}

meter.write16bits(0x24, 0x0000);  // AVRMSGAIN
meter.write16bits(0x25, 0x0000);  // BVRMSGAIN
meter.write16bits(0x26, 0x0000);  // CVRMSGAIN
meter.write16bits(0x27, 0x0000);  // AIGAIN
meter.write16bits(0x28, 0x0000);  // BIGAIN
meter.write16bits(0x29, 0x0000);  // CIGAIN
delay(10);

// ───────────────────────────────────────────────────────────
// PASUL 5: CONFIGURARE DIVIZORI
// ───────────────────────────────────────────────────────────
Serial.println("5️⃣ Configurare divizori...");

meter.write8bits(WDIV, 0x02);
meter.write8bits(VARDIV, 0x02);
meter.write8bits(VADIV, 0x02);
delay(20);

uint8_t wdiv_check = meter.read8bits(WDIV);
if (wdiv_check != 0x02) {
  Serial.printf("  ⚠️  WDIV greșit (0x%02X), rescriu...\n", wdiv_check);
  meter.write8bits(WDIV, 0x02);
  meter.write8bits(VARDIV, 0x02);
  meter.write8bits(VADIV, 0x02);
  delay(20);
}

// ───────────────────────────────────────────────────────────
// PASUL 6: RESETARE OFFSET REGISTERS
// ───────────────────────────────────────────────────────────
Serial.println("6️⃣ Resetare offset registers...");

meter.write16bits(0x33, 0);  // AVRMSOS
meter.write16bits(0x34, 0);  // BVRMSOS
meter.write16bits(0x35, 0);  // CVRMSOS
meter.write16bits(0x36, 0);  // AIRMSOS
meter.write16bits(0x37, 0);  // BIRMSOS
meter.write16bits(0x38, 0);  // CIRMSOS
delay(10);

meter.resetStatus();

// Activează IRQ pe LENERGY (acumulare LINECYC completă)
meter.write24bits(MASK, LENERGY);  // 0x001000 — 3 bytes corecți
meter.resetStatus();  // curăță orice flag rezidual
// Init semaphore + pin IRQ
xAdeIrqSem = xSemaphoreCreateBinary();
pinMode(IRQ_PIN, INPUT_PULLUP);
attachInterrupt(digitalPinToInterrupt(IRQ_PIN), adeIrqISR, FALLING);
// Init WP pin NVRAM
pinMode(NVRAM_WP_PIN, OUTPUT);
digitalWrite(NVRAM_WP_PIN, LOW);

delay(50);

// ───────────────────────────────────────────────────────────
// PASUL 7: VERIFICARE FINALĂ REGISTRE
// ───────────────────────────────────────────────────────────
Serial.println("7️⃣ Verificare configurație finală...\n");
delay(200);

uint8_t opmode_final = meter.read8bits(OPMODE);
uint8_t mmode_final = meter.read8bits(MMODE);
uint8_t gain_final = meter.read8bits(GAIN);
uint8_t wdiv_final = meter.read8bits(WDIV);
uint8_t vardiv_final = meter.read8bits(VARDIV);
uint8_t vadiv_final = meter.read8bits(VADIV);

Serial.println("────── Registre finale ──────");
Serial.printf("  OPMODE: 0x%02X\n", opmode_final);
Serial.printf("  MMODE:  0x%02X\n", mmode_final);
Serial.printf("  GAIN:   0x%02X\n", gain_final);
Serial.printf("  WDIV:   0x%02X\n", wdiv_final);
Serial.printf("  VARDIV: 0x%02X\n", vardiv_final);
Serial.printf("  VADIV:  0x%02X\n", vadiv_final);

if (opmode_final != DISCF) {
  Serial.println("\n⚠️  OPMODE încă greșit! Ultim attempt...");
  meter.write8bits(OPMODE, DISCF);
  delay(50);
  opmode_final = meter.read8bits(OPMODE);
  Serial.printf("  → OPMODE final: 0x%02X\n", opmode_final);
}

Serial.println("─────────────────────────────\n");

// ───────────────────────────────────────────────────────────
// PASUL 8: TEST SPI
// ───────────────────────────────────────────────────────────
Serial.println("8️⃣ Test comunicare SPI...");

meter.write16bits(LINECYC, 0x1234);
delay(100);  // 100ms, nu 10ms!
uint16_t test_val = meter.read16bits(LINECYC);
Serial.printf("LINECYC după 100ms: 0x%04X\n", test_val);

delay(100);
test_val = meter.read16bits(LINECYC);
Serial.printf("LINECYC după încă 100ms: 0x%04X\n", test_val);

if (test_val == 0x1234) {
  Serial.println("  ✅ SPI OK");
} else {
  Serial.printf("  ⚠️  SPI WARNING - citit=0x%04X\n", test_val);
}

// ───────────────────────────────────────────────────────────
// PASUL 9: CITIRE RAW
// ───────────────────────────────────────────────────────────
Serial.println("\n9️⃣ Citire valori RAW...");
delay(500);

long raw_vrms_a = meter.read24bits(AVRMS);
long raw_vrms_b = meter.read24bits(BVRMS);
long raw_vrms_c = meter.read24bits(CVRMS);
long raw_irms_a = meter.read24bits(AIRMS);
long raw_irms_b = meter.read24bits(BIRMS);
long raw_irms_c = meter.read24bits(CIRMS);
uint16_t freq_raw = meter.read16bits(FREQ);

Serial.println("\n────── Valori RAW ──────");
Serial.printf("  VRMS: A=%ld  B=%ld  C=%ld\n", raw_vrms_a, raw_vrms_b, raw_vrms_c);
Serial.printf("  IRMS: A=%ld  B=%ld  C=%ld\n", raw_irms_a, raw_irms_b, raw_irms_c);
Serial.printf("  FREQ: %u (%.2f Hz)\n", freq_raw, freq_raw / FREQ_CAL);
Serial.println("────────────────────────\n");

// ═══════════════════════════════════════════════════════════
// PASUL 10: PORNIRE ACUMULARE - ACEASTA LIPSEA!!!
// ═══════════════════════════════════════════════════════════
Serial.println("🔟 Pornire acumulare energie...");
// Scrie din nou LINECYC pentru a porni acumularea
meter.write16bits(LINECYC, 30);
delay(50);
Serial.println("  ✅ Acumulare pornită (LINECYC rescris)");

// Verifică dacă acumularea a pornit
uint16_t linecyc_check = meter.read16bits(LINECYC);
Serial.printf("  LINECYC = %u (trebuie să fie 30)\n", linecyc_check);
  // ═══════════════════════════════════════════════════════════
  // 💾 ÎNCĂRCARE SETĂRI DIN NVRAM
  // ═══════════════════════════════════════════════════════════
  Serial.println("Încărcare setări din NVRAM...");
  loadSettingsFromNVRAM();
  
DBG_PRINTLN("\n🔍 TEST VRMS/IRMS get vs read24 (dupa NVRAM load)\n");



long vr_get = meter.getVRMS(PHASE_A);
long vr_raw = meter.read24bits(AVRMS);

long ir_get = meter.getIRMS(PHASE_A);
long ir_raw = meter.read24bits(AIRMS);


// PASUL 10: AUTO-CALIBRARE (DIAGNOSTIC) - NU atinge offseturile!
// ═══════════════════════════════════════════════════════════
Serial.println("🔟 Pasul 10: Diagnostic RAW + scale (fara offset overwrite)...");
delay(200);

// Dacă ai făcut deja calibrarea de fabrică, nu mai are voie să schimbe nimic.
if (factoryCalDone) {
  Serial.println("🔒 factoryCalDone=TRUE -> Pasul 10 ruleaza DOAR diagnostic (fara scriere in NVRAM).");
}

const int N_SAMPLES = 10;

long sum_vrms_a = 0, sum_vrms_b = 0, sum_vrms_c = 0;
long sum_irms_a = 0, sum_irms_b = 0, sum_irms_c = 0;

int valid = 0;

for (int i = 0; i < N_SAMPLES; i++) {

  long vrA = meter.read24bits(AVRMS);
  long vrB = meter.read24bits(BVRMS);
  long vrC = meter.read24bits(CVRMS);

  long irA = meter.read24bits(AIRMS);
  long irB = meter.read24bits(BIRMS);
  long irC = meter.read24bits(CIRMS);


  // filtre simple anti-citiri invalide (optional)
  // 0xFFFFFF e tipic pentru "no data / bus issue" la 24-bit
  if (vrA == 0xFFFFFF || vrB == 0xFFFFFF || vrC == 0xFFFFFF ||
      irA == 0xFFFFFF || irB == 0xFFFFFF || irC == 0xFFFFFF) {
    delay(50);
    continue;
  }

  sum_vrms_a += vrA;
  sum_vrms_b += vrB;
  sum_vrms_c += vrC;

  sum_irms_a += irA;
  sum_irms_b += irB;
  sum_irms_c += irC;

  valid++;
  delay(50);
}

if (valid == 0) {
  Serial.println("❌ Pasul 10: Niciun esantion valid! Verifica SPI/ADE.");
} else {
  float avg_vrms_a = (float)sum_vrms_a / (float)valid;
  float avg_vrms_b = (float)sum_vrms_b / (float)valid;
  float avg_vrms_c = (float)sum_vrms_c / (float)valid;

  float avg_irms_a = (float)sum_irms_a / (float)valid;
  float avg_irms_b = (float)sum_irms_b / (float)valid;
  float avg_irms_c = (float)sum_irms_c / (float)valid;



  // ✅ Offseturile sunt COUNTS din factory calibration (nu le recalcula aici!)
  // Dacă nu ai factory offsets încă, doar afișează ce există.
  Serial.println("\n────── RAW medii (counts) ──────");
  Serial.printf("  VRMS raw: A=%.0f  B=%.0f  C=%.0f  (valid=%d)\n", avg_vrms_a, avg_vrms_b, avg_vrms_c, valid);
  Serial.printf("  IRMS raw: A=%.0f  B=%.0f  C=%.0f\n", avg_irms_a, avg_irms_b, avg_irms_c);

  Serial.println("\n────── Offset factory (counts) ──────");
  Serial.printf("  V_off: A=%.0f  B=%.0f  C=%.0f\n", voltageOffsetA, voltageOffsetB, voltageOffsetC);
  Serial.printf("  I_off: A=%.0f  B=%.0f  C=%.0f\n", currentOffsetA, currentOffsetB, currentOffsetC);

  // calculează diferența în counts (asta e ce trebuie să fie aproape de 0 la 0V/0A)
  float dvA = avg_vrms_a - voltageOffsetA;
  float dvB = avg_vrms_b - voltageOffsetB;
  float dvC = avg_vrms_c - voltageOffsetC;

  float diA = avg_irms_a - currentOffsetA;
  float diB = avg_irms_b - currentOffsetB;
  float diC = avg_irms_c - currentOffsetC;

  Serial.println("\n────── Diferenta (raw - offset) counts ──────");
  Serial.printf("  dV: A=%.0f  B=%.0f  C=%.0f\n", dvA, dvB, dvC);
  Serial.printf("  dI: A=%.0f  B=%.0f  C=%.0f\n", diA, diB, diC);

  // (opțional) conversie doar ca diagnostic, NU ca offset:
  float CT_Ratio = (float)CT_Primary / (float)CT_Secondary;

  float vA_diag = (dvA > 0 ? dvA : 0) * VRMS_SCALE_A * voltageFactor;
  float vB_diag = (dvB > 0 ? dvB : 0) * VRMS_SCALE_B * voltageFactor;
  float vC_diag = (dvC > 0 ? dvC : 0) * VRMS_SCALE_C * voltageFactor;

  float iA_diag = (diA > 0 ? diA : 0) * IRMS_SCALE_A * CT_Ratio * currentFactor;
  float iB_diag = (diB > 0 ? diB : 0) * IRMS_SCALE_B * CT_Ratio * currentFactor;
  float iC_diag = (diC > 0 ? diC : 0) * IRMS_SCALE_C * CT_Ratio * currentFactor;

  Serial.println("\n────── Diagnostic (NU salvat) ──────");
  Serial.printf("  Vdiag: A=%.2fV  B=%.2fV  C=%.2fV\n", vA_diag, vB_diag, vC_diag);
  Serial.printf("  Idiag: A=%.2fA  B=%.2fA  C=%.2fA\n", iA_diag, iB_diag, iC_diag);
  Serial.println("──────────────────────────────\n");

  // ❌ NU salva offseturi aici. Nici măcar dacă factoryCalDone==false.
  // Offseturile se salvează DOAR în calibrazioneAutomaticaOffset().
}

Serial.println("✅ Pasul 10 complet.\n");
Serial.println("═══════════════════════════════════════════════════════════\n");



DBG_PRINTf("VRMS get=%ld  read24=%ld\n", vr_get, vr_raw);
DBG_PRINTf("IRMS get=%ld  read24=%ld\n", ir_get, ir_raw);
DBG_PRINTLN("");


  Serial.println("\n📋 Setări încărcate:");
  Serial.printf("  • Voltage Factor: %.3f\n", voltageFactor);
  Serial.printf("  • Current Factor: %.3f\n", currentFactor);
  Serial.printf("  • Voltage Offsets: A=%.2f, B=%.2f, C=%.2f\n", 
                voltageOffsetA, voltageOffsetB, voltageOffsetC);
  Serial.printf("  • Current Offsets: A=%.3f, B=%.3f, C=%.3f\n", 
                currentOffsetA, currentOffsetB, currentOffsetC);
  Serial.println();
DBG_PRINTf("   Offset V_A/B/C (counts): %.0f / %.0f / %.0f\n", voltageOffsetA, voltageOffsetB, voltageOffsetC);
DBG_PRINTf("   Offset I_A/B/C (counts): %.0f / %.0f / %.0f\n", currentOffsetA, currentOffsetB, currentOffsetC);




  // ═══════════════════════════════════════════════════════════
  // 📡 PORNIRE WiFi ACCESS POINT (ÎNTOTDEAUNA ACTIV)
  // ═══════════════════════════════════════════════════════════
  Serial.println("┌───────────────────────────────────────┐");
  Serial.println("│    WiFi Access Point                  │");
  Serial.println("└───────────────────────────────────────┘");
  
  startWebServer();
  
  Serial.println("✅ WiFi AP activ");
  Serial.printf("   SSID: EBS-Orbital-Config\n");
  Serial.printf("   IP:   %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("   Web:  http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🌐 INIȚIALIZARE ETHERNET (OPȚIONAL)
  // ═══════════════════════════════════════════════════════════
  Serial.println("┌───────────────────────────────────────┐");
  Serial.println("│    Ethernet W5500 (Optional)          │");
  Serial.println("└───────────────────────────────────────┘");
  
  if (!ETH.beginSPI(W5500_MISO, W5500_MOSI, W5500_SCLK, W5500_CS, W5500_RST, W5500_IRQ)) {
    Serial.println("⚠️  W5500 nu a fost detectat");
    Serial.println("   Continuare doar cu WiFi AP");
    ethernetConnected = false;
  } else {
    Serial.println("✅ W5500 detectat - așteptare IP DHCP...");
    
    int maxTries = 4;
    while (ETH.localIP() == IPAddress(0, 0, 0, 0) && maxTries > 0) {
      Serial.printf("   Încercare %d/4...\n", 5 - maxTries);
      delay(1000);
      maxTries--;
    }

    if (ETH.localIP() == IPAddress(0, 0, 0, 0)) {
      Serial.println("⚠️  Ethernet: Timeout alocare IP");
      Serial.println("   Verifică cablul Ethernet");
      ethernetConnected = false;
    } else {
      Serial.println("✅ Ethernet conectat");
      Serial.printf("   IP:  %s\n", ETH.localIP().toString().c_str());
      Serial.printf("   Web: http://%s\n", ETH.localIP().toString().c_str());
      ethernetConnected = true;

      // ═══════════════════════════════════════════════════════
      // 🔗 TESTARE CONEXIUNE MODBUS TCP
      // ═══════════════════════════════════════════════════════
      Serial.println();
      Serial.println("Testare conexiune Modbus TCP...");
      Serial.printf("  Target: %s:%d\n", MODBUS_SLAVE_IP.toString().c_str(), MODBUS_PORT);
      
      client.stop();
      delay(100);

      if (client.connect(MODBUS_SLAVE_IP, MODBUS_PORT)) {
        Serial.println("✅ Modbus conectat la pornire");
        modbusConnected = true;
        client.setTimeout(2000);
        g_lastModbusTxMs = millis();
      } else {
        Serial.println("⚠️  Modbus nu răspunde");
        Serial.println("   Va încerca reconectare automată");
        modbusConnected = false;
      }

      // Pornește task-ul Modbus doar dacă Ethernet e activ
      Serial.println();
      Serial.println("Pornire Modbus Task...");
      xTaskCreatePinnedToCore(ModbusTask, "ModbusTask", 4096, NULL, 1, NULL, 0);
      Serial.println("✅ Modbus Task activ (Core 0)");
    }
  }
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // 🚀 PORNIRE TASK-URI RTOS
  // ═══════════════════════════════════════════════════════════
  Serial.println("┌───────────────────────────────────────┐");
  Serial.println("│    RTOS Tasks                         │");
  Serial.println("└───────────────────────────────────────┘");
  
  xTaskCreatePinnedToCore(ModbusReconnectTask, "ModbusReconnect", 4096, NULL, 1, 
                          &TaskHandle_ModbusReconnect, 1);
  Serial.println("✅ Modbus Reconnect Task (Core 1)");
  
  xTaskCreatePinnedToCore(updatePWMFromWatt, "PWM_Control", 8192, NULL, 1, 
                          &TaskHandle_Core2, 1);
  Serial.println("✅ PWM Control Task (Core 1)");
  


  xTaskCreatePinnedToCore(WebTask, "WebTask", 8192, NULL, 1, &WebTaskHandle, 0);// 🔥 CORE 0
  Serial.println("✅ Web Task (Core 0)");
  Serial.println();

  // ═══════════════════════════════════════════════════════════
  // ✅ SETUP COMPLET - SUMARUL FINAL
  // ═══════════════════════════════════════════════════════════
  Serial.println("╔═══════════════════════════════════════════════════════════╗");
  Serial.println("║              ✅ SETUP COMPLET - SISTEM ACTIV              ║");
  Serial.println("╚═══════════════════════════════════════════════════════════╝");
  Serial.println();
  Serial.println("📊 Status:");
Serial.printf("   • Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("   • PSRAM:     %d bytes\n", ESP.getPsramSize());
  Serial.println();
  Serial.println("🌐 Conexiuni:");
  Serial.printf("   • WiFi AP:   %s (întotdeauna activ)\n", 
                WiFi.softAPIP().toString().c_str());
  if (ethernetConnected) {
    Serial.printf("   • Ethernet:  %s (activ)\n", ETH.localIP().toString().c_str());
    Serial.printf("   • Modbus:    %s\n", modbusConnected ? "Conectat" : "Deconectat");
  } else {
    Serial.println("   • Ethernet:  Inactiv");
  }
  Serial.println();
  Serial.println("🔧 Pentru comenzi debug, tastează în Serial Monitor:");
  Serial.println("   S - Status sistem");
  Serial.println("   M - Reconectare Modbus");
  Serial.println("   R - Reset W5500");
  Serial.println("   X - Restart ESP32");
  Serial.println();
  Serial.println("════════════════════════════════════════════════════════════");
  Serial.println();
  
  // Actualizează display-ul OLED
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.println("READY!");
    display.setTextSize(1);
    display.setCursor(5, 35);
    display.printf("WiFi: %s", WiFi.softAPIP().toString().c_str());
    if (ethernetConnected) {
      display.setCursor(5, 50);
      display.printf("ETH:  %s", ETH.localIP().toString().c_str());
    }
    display.display();
    xSemaphoreGive(i2cMutex);
  }

xTaskCreatePinnedToCore(
    TaskMeter,
    "TaskMeter",
    8192,
    NULL,
    2,
    NULL,
    1
);
// Calibrare baseline touch după init complet
{
  uint64_t sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += touchRead(TOUCH_PIN);
    delay(10);
  }
  g_touchBaseline = (uint32_t)(sum / 20);
  Serial.printf("✅ Touch baseline calibrat: %lu\n", g_touchBaseline);
}
displayOn = true;
lastTouchTime = millis();
// ← ABIA ACUM pornești OledTask, după ce baseline e setat!
  xTaskCreatePinnedToCore(OledTask, "OledTask", 4096, nullptr, 1, nullptr, 1);
  Serial.println("✅ OLED Display Task (Core 1)");
  Serial.println();
  delay(2000);  // Lasă mesajul READY vizibil
} // ⬅️ ACEASTA este singura acoladă de închidere a funcției setup()

//================== Loop ===================
void loop() {
  const uint32_t now_ms = millis();

  if (Serial.available()) {
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "cal") calibrazioneAutomaticaOffset();
}


  // ✅ CONFIRMARE FIRMWARE (TREBUIE SĂ FIE LA ÎNCEPUT, O SINGURĂ DATĂ)
  if (!fwConfirmed && millis() > 15000) {  // După 15 secunde de funcționare stabilă
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;

    if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
      Serial.printf("🔍 RUN partition: %s, state=%s\n", 
                    run->label, otaStateName(st));

      if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("✅ Firmware CONFIRMED - rollback cancelled!");
      } else {
        Serial.println("ℹ️ Firmware already validated, no action needed");
      }
    } else {
      Serial.println("⚠️ Could not get OTA partition state");
    }

    fwConfirmed = true;  // Marcăm că am verificat
  }

  g_loopCounter++;
  if (now_ms - g_lastLoopTime >= 5000) {
    DBG_PRINTf("💓 System active: %lu iterations (netBusy: %s)\n",
               g_loopCounter, g_netBusy ? "YES" : "NO");
    g_loopCounter = 0;
    g_lastLoopTime = now_ms;
  }

  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c=='s' || c=='S') {
      DBG_PRINTLN("📊 System Status:");
      DBG_PRINTf("  Uptime: %lu min\n", millis() / 60000);
      DBG_PRINTf("  Free Heap: %d bytes\n", ESP.getFreeHeap());
      DBG_PRINTf("  WiFi AP: %s\n", WiFi.softAPIP().toString().c_str());
      DBG_PRINTf("  Ethernet: %s\n", ethernetConnected ? "OK" : "NO");
      DBG_PRINTf("  Modbus: %s\n", modbusConnected ? "OK" : "NO");
    }
    else if (c=='r' || c=='R') {
      DBG_PRINTLN("🔥 Manual hard reset W5500 requested");
      g_hardResetPending = true;
    }
    else if (c=='m' || c=='M') {
      DBG_PRINTLN("🔄 Manual reconnect - stop client");
      if (client.connected()) client.stop();
      modbusConnected = false;
    }
    else if (c=='x' || c=='X') {
      DBG_PRINTLN("🔄 System restart in 3 seconds...");
      delay(3000);
      ESP.restart();
    }
  }

  static uint32_t lastMeasurement   = 0;
  static uint32_t lastDisplayUpdate = 0;
  static uint32_t lastDebugPrint    = 0;
  static uint32_t lastEthPoll       = 0;



  // ═══════════════════════════════════════════════════════════
  // ⚡ CITIRI ADE7758 - CU CONSTANTE SEPARATE PER FAZĂ
  // ═══════════════════════════════════════════════════════════


  // ═══════════════════════════════════════════════════════════
  // 📊 ACUMULARE ENERGIE
  // ═══════════════════════════════════════════════════════════
  if ((now_ms - previousMillis) >= 1000) {
    previousMillis = now_ms;
    if (Watt > 0.1f) KWh += (Watt / 1000.0f) / 3600.0f;  // W→kWh
  }

  // ═══════════════════════════════════════════════════════════
  // 🌐 CHECK ETHERNET LINK STATE — PHYSICAL TRUTH
  // ═══════════════════════════════════════════════════════════
  uint32_t tEth = micros();
  bool currentLinkUp = ETH.linkUp();
  bool currentHasIP = (ETH.localIP() != IPAddress(0,0,0,0));

  if (currentLinkUp && currentHasIP) {
    if (!ethernetConnected) {
      DBG_PRINT("📡 Ethernet LINK RESTORED: ");
      DBG_PRINTLN(ETH.localIP());
      ethernetConnected = true;
    }
  } else {
    if (ethernetConnected || modbusConnected) {
      DBG_PRINTLN("🚨 PHYSICAL CABLE CUT — killing Modbus state NOW");
      if (client.connected()) client.stop();
      ethernetConnected = false;
      modbusConnected = false;
    }
  }
  // profUpdate(prof.maxEthUs, micros() - tEth);
  // ═══════════════════════════════════════════════════════════
  // 🌐 WEB SERVER (WiFi AP întotdeauna activ)
  // ═══════════════════════════════════════════════════════════
  uint32_t t0 = micros();
 // handleWebClient();
  profUpdate(prof.maxWebUs, micros() - t0);

  // ═══════════════════════════════════════════════════════════
  // 🐛 DEBUG PERIODIC
  // ═══════════════════════════════════════════════════════════
  if (now_ms - lastDebugPrint >= DEBUG_PRINT_INTERVAL) {
    DBG_PRINTLN("-----------------------------");
    DBG_PRINTf("ADE Status: %.2f Hz, %.2f W, %.2f kWh, Pf=%.2f\n",
               Frequency, Watt, KWh, Pf);
    DBG_PRINTf("Voltages: A=%.1fV, B=%.1fV, C=%.1fV\n", v_a, v_b, v_c);
    DBG_PRINTf("Currents: A=%.2fA, B=%.2fA, C=%.2fA\n", c_a, c_b, c_c);
    DBG_PRINTf("Network Status: WiFi=%s, Ethernet=%s, Modbus=%s\n",
               WiFi.softAPIP().toString().c_str(),
               ethernetConnected ? "OK" : "NO",
               modbusConnected ? "OK" : "NO");
//float wattInstA = meter.getWatt(PHASE_A);

//float pfCalc = (VA_a > 0.001f) ? (wattInstA / VA_a) : 0.0f;

float vaDbg  = (float)meter.getVA(PHASE_A) * VA_CAL;
float pfCalc = (VA_a > 0.001f) ? (WattA / VA_a) : 0.0f;

DBG_PRINTf("[ADE] VRMS=%.2f  IRMS=%.3f  VA=%.2f  W=%.2f  WattA=%.2f  W/VA=%.4f\n",
           v_a, c_a, vaDbg, WattA, WattA, pfCalc);
    DBG_PRINTLN("-----------------------------");

    // ← adaugă doar aceste 3 linii:
    unsigned long ade_status = meter.read24bits(STATUS);
    Serial.printf("ADE STATUS: 0x%06lX | LENERGY=%s | IO15=%d\n",
        ade_status, (ade_status & LENERGY) ? "SET✅" : "clear❌", digitalRead(IRQ_PIN));
    meter.resetStatus();

    lastDebugPrint = now_ms;
  }

if (now_ms - prof.lastPrintMs > 2000) {
  prof.lastPrintMs = now_ms;
  Serial.printf("PROF max(us): web=%u meter=%u\n",
                prof.maxWebUs,
                prof.maxMeterUs);

  prof.maxWebUs = 0;
  prof.maxMeterUs = 0;
}

  yield();
  delay(1);
  
  // 🐛 DEBUG RAW VALUES (OPȚIONAL - comentează după testare)
  // uint32_t ra = meter.getVRMS(PHASE_A);
  // uint32_t ia = meter.getIRMS(PHASE_A);
  // Serial.printf("RAW VRMS_A=0x%06lX  IRMS_A=0x%06lX\n", ra, ia);
}



