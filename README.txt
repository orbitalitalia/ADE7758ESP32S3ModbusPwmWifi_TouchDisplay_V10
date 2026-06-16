═══════════════════════════════════════════════════════════════
  ESP32-S3-N16R8 OTA ROLLBACK - INSTRUCȚIUNI DE INSTALARE
═══════════════════════════════════════════════════════════════

📦 CONȚINUT PACHET:
  ✓ sdkconfig        - Configurare rollback OTA
  ✓ partitions.csv   - Schema partiții optimizată 16MB
  ✓ README.txt       - Acest fișier

═══════════════════════════════════════════════════════════════
🔧 INSTALARE ÎN ARDUINO IDE
═══════════════════════════════════════════════════════════════

PASUL 1: COPIAZĂ FIȘIERELE
──────────────────────────────────────────────────────────────
Copiază fișierele în folderul unde ai sketch-ul tău .ino:

Exemplu:
  C:\Users\Dan\Documents\Arduino\MyProject\
    ├── MyProject.ino       ← Firmware-ul tău
    ├── sdkconfig           ← COPIAZĂ AICI
    ├── partitions.csv      ← COPIAZĂ AICI (opțional)
    ├── globals.h
    ├── globals.cpp
    └── ...alte fișiere

PASUL 2: SETĂRI ARDUINO IDE
──────────────────────────────────────────────────────────────
Deschide Arduino IDE și configurează:

Tools → Board:              "ESP32S3 Dev Module"
Tools → Flash Size:         "16MB (128Mb)"
Tools → Partition Scheme:   "16M Flash (3MB APP/9.9MB FATFS)"
                            SAU "Default 4MB with spiffs" 
                            (depinde de versiunea Arduino)
Tools → PSRAM:              "OPI PSRAM"
Tools → Upload Speed:       "921600"
Tools → USB CDC On Boot:    "Enabled"

PASUL 3: PRIMA ÎNCĂRCARE (prin USB)
──────────────────────────────────────────────────────────────
1. Conectează ESP32-S3 la USB
2. Apasă butonul UPLOAD în Arduino IDE
3. Verifică în Serial Monitor (115200 baud):
   
   ✅ Trebuie să vezi:
      "✅ Rollback: ENABLED"
      "OTA_0: 0x010000 - 3072 KB"
      "OTA_1: 0x310000 - 3072 KB"

   ❌ Dacă vezi "Rollback: DISABLED":
      - Verifică că ai copiat fișierul sdkconfig
      - Recompilează (Sketch → Verify/Compile)
      - Încarcă din nou

═══════════════════════════════════════════════════════════════
🧪 TESTARE ROLLBACK
═══════════════════════════════════════════════════════════════

FIRMWARE PRINCIPAL (cod existent)
──────────────────────────────────────────────────────────────
În funcția loop() trebuie să confirmi firmware-ul după 15 sec:

void loop() {
  // ... codul tău existent ...
  
  // ✅ CONFIRMARE FIRMWARE (pune la început în loop)
  static bool fwConfirmed = false;
  if (!fwConfirmed && millis() > 15000) {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    
    if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
      if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("✅ Firmware confirmed!");
      }
    }
    fwConfirmed = true;
  }
  
  // ... restul codului ...
}

FIRMWARE DE TEST (pentru verificare rollback)
──────────────────────────────────────────────────────────────
Creează un sketch nou cu acest cod:

#include "esp_ota_ops.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n🔥 TEST ROLLBACK");
  
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  
  if (esp_ota_get_state_partition(run, &state) == ESP_OK) {
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("State: PENDING_VERIFY");
      Serial.println("Crash în 5 secunde...");
      
      for (int i = 5; i > 0; i--) {
        Serial.printf("%d...\n", i);
        delay(1000);
      }
      
      Serial.println("💥 RESTART!");
      ESP.restart();  // VA FACE ROLLBACK!
    }
  }
}

void loop() {}

PROCEDURA DE TEST:
──────────────────────────────────────────────────────────────
1. Încarcă firmware-ul PRINCIPAL prin USB
2. Lasă-l să ruleze > 15 secunde (confirmă firmware)
3. Încarcă firmware-ul DE TEST prin OTA (nu USB!)
4. Urmărește Serial Monitor:
   - Ar trebui să vadă "PENDING_VERIFY"
   - Numără invers 5...1
   - Face restart
   - Revine AUTOMAT la firmware-ul principal!

═══════════════════════════════════════════════════════════════
⚠️  TROUBLESHOOTING
═══════════════════════════════════════════════════════════════

PROBLEMA: "Rollback: DISABLED" în Serial Monitor
SOLUȚIE:
  1. Verifică că sdkconfig este în folderul corect
  2. Închide și redeschide Arduino IDE
  3. Sketch → Verify/Compile (forțează recompilare)
  4. Upload din nou

PROBLEMA: Nu există OTA_0 și OTA_1
SOLUȚIE:
  1. Tools → Partition Scheme
  2. Alege una care include "OTA" în nume
  3. SAU folosește partitions.csv (vezi mai jos)

PROBLEMA: OTA upload eșuează
SOLUȚIE:
  1. Verifică că ESP32 este conectat la rețea
  2. IP-ul este corect (192.168.1.177 sau 192.168.1.20)
  3. Portul WebSocket 81 nu este blocat de firewall

═══════════════════════════════════════════════════════════════
🔧 FOLOSIRE partitions.csv (OPȚIONAL)
═══════════════════════════════════════════════════════════════

Dacă partition scheme-ul implicit nu funcționează:

1. Copiază partitions.csv în folderul sketch-ului
2. Arduino IDE va detecta automat fișierul
3. În Tools → Partition Scheme va apărea "Custom"
4. Selectează "Custom"
5. Recompilează și încarcă

NOTĂ: După schimbarea partition scheme, ESP32 va fi șters 
complet! Vei pierde toate datele din SPIFFS/NVS.

═══════════════════════════════════════════════════════════════
📊 VERIFICARE CONFIGURAȚIE
═══════════════════════════════════════════════════════════════

Folosește acest cod pentru a verifica totul:

#include "esp_ota_ops.h"
#include "esp_partition.h"

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== VERIFICARE OTA CONFIG ===\n");
  
  #ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    Serial.println("✅ Rollback ENABLED");
  #else
    Serial.println("❌ Rollback DISABLED");
  #endif
  
  const esp_partition_t* ota0 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t* ota1 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  
  if (ota0 && ota1) {
    Serial.println("✅ Partiții OTA configurate");
    Serial.printf("   OTA_0: %u KB\n", ota0->size / 1024);
    Serial.printf("   OTA_1: %u KB\n", ota1->size / 1024);
  } else {
    Serial.println("❌ Lipsesc partiții OTA!");
  }
  
  const esp_partition_t* run = esp_ota_get_running_partition();
  Serial.printf("\nRunning: %s\n", run->label);
}

void loop() {}

═══════════════════════════════════════════════════════════════
📞 SUPORT
═══════════════════════════════════════════════════════════════

Dacă întâmpini probleme:
1. Rulează codul de verificare de mai sus
2. Trimite output-ul Serial Monitor
3. Menționează versiunea Arduino IDE și esp32 package

Versiuni testate:
  ✓ Arduino IDE 2.x
  ✓ esp32 board package 2.0.14+
  ✓ ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)

═══════════════════════════════════════════════════════════════
✅ CHECKLIST FINAL
═══════════════════════════════════════════════════════════════

Înainte de a testa OTA cu rollback, verifică:

[ ] sdkconfig este în folderul sketch-ului
[ ] Arduino IDE Tools → Flash Size = "16MB (128Mb)"
[ ] Arduino IDE Tools → PSRAM = "OPI PSRAM"
[ ] Partition Scheme include "OTA" SAU folosești partitions.csv
[ ] Prima încărcare făcută prin USB
[ ] Serial Monitor arată "✅ Rollback: ENABLED"
[ ] Serial Monitor arată "OTA_0" și "OTA_1" configurate
[ ] Firmware-ul principal confirmă după 15 secunde în loop()
[ ] Test rollback făcut prin OTA (NU prin USB!)

═══════════════════════════════════════════════════════════════

✅ V07 – Rezumat funcțional
🔹 Măsurători & control (nemodificate față de V06)

ADE7758: tensiune, curent, puteri (activă / reactivă / aparentă), energie

Control PWM + Step2–Step4

Logică de disipare progresivă

OLED actualizat periodic

Modbus TCP prin W5500 (ETHClass)

🔹 Nou în V07

Potenza rete

Citită din Modbus Input Register 13

Actualizare periodică

Afișată în Web UI, sub Potenza Totale

Trimisa prin WebSocket (power_rete)

Versiune software V07

Afișată la boot pe Serial

Disponibilă în Web UI / WebSocket

🔹 Web / Interfață

Web UI complet funcțional

WebSocket pentru update în timp real:

valori electrice

stări Step

Potenza rete

Accesibilitate HTML păstrată

🔹 OTA Update (ca în V06)

OTA Web Update activ

Endpoint:

POST /update

GET /ota_status

Autentificare:

user: admin

pass: orbital123

Upload firmware .bin din browser

Confirmare OTA + rollback ESP-IDF

🔹 Stabilitate

V06 păstrat ca backup

V07 este complet identificabil (Serial + UI)

Fără breaking changes față de V06
