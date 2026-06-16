#include "Arduino.h"
#include <SPI.h>
#include "ADE7758.h"
#include "globals.h"

// ==================== CONSTRUCTOR ====================

ADE7758::ADE7758(){
}

// ==================== INIȚIALIZARE ====================

void ADE7758::Init(void) {
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);

    SPI.begin(8, 6, 7, 5); // CLK=8, MISO=6, MOSI=7, CS=5 pe ESP32
    SPI.setDataMode(SPI_MODE1);
    SPI.setBitOrder(MSBFIRST);
    delay(10);

    // ⚙️ CONFIGURARE MINIMĂ - restul se face în setup()
    // Doar setări de bază necesare pentru comunicare
    write8bits(MMODE, 0x00);        // măsurători de putere simultane
    write8bits(WAVMODE, 0x00);      // waveform mod dezactivat
    write8bits(LCYCMODE, 0x38);     // activează toate măsurările în ciclu
    write16bits(LINECYC, 30);       // adună pe 30 semiperioade

    delay(50);
}

void ADE7758::setSPI(void) {
    SPI.setDataMode(SPI_MODE1);
    SPI.setClockDivider(SPI_CLOCK_DIV8);
    SPI.setBitOrder(MSBFIRST);
    delay(2);
}

void ADE7758::closeSPI(void) {
    SPI.setDataMode(SPI_MODE1);  // ✅ CORECT - păstrează MODE1
    SPI.setClockDivider(SPI_CLOCK_DIV8);
    SPI.setBitOrder(MSBFIRST);
    delay(2);
}

// ==================== FUNCȚII PRIVATE ====================

void ADE7758::enable(){
    digitalWrite(5, LOW);
     delayMicroseconds(50);  // ← ADAUGĂ
}

void ADE7758::disable(){
    digitalWrite(5, HIGH);
}

// ==================== FUNCȚII DE CITIRE LOW-LEVEL ====================

unsigned char ADE7758::read8bits(char reg) {
    static unsigned char lastVal = 0;

    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2))) {
        SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE1));

        enable();
        delayMicroseconds(5);  // ✅ Crescut pentru CS setup

        // Asigură READ (bit7=0)
        SPI.transfer((uint8_t)reg & 0x7F);
        delayMicroseconds(5);  // ✅ Crescut pentru procesor ADE

        uint8_t b0 = SPI.transfer(0x00);

        // CRITIC: hold înainte de CS HIGH
        delayMicroseconds(5);  // ✅ CRESCUT - min 4μs per datasheet

        disable();
        delayMicroseconds(5);  // ✅ ADĂUGAT - hold după CS HIGH

        SPI.endTransaction();
        xSemaphoreGive(spiMutex);

        lastVal = b0;
    }

    return lastVal;
}

unsigned int ADE7758::read16bits(char reg) {
    unsigned int result = 0;
    
    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
        
        digitalWrite(5, LOW);
        delayMicroseconds(10);
        
        // Trimite adresa cu bitul de READ
        SPI.transfer(reg & 0x7F);
        delayMicroseconds(10);  // Așteaptă puțin, dar fără clock-uri
        
        // 🔥 DIRECT 16 clock-uri pentru date
        uint8_t msb = SPI.transfer(0x00);  // clock 8-15: MSB
        delayMicroseconds(5);
        
        uint8_t lsb = SPI.transfer(0x00);  // clock 16-23: LSB
        delayMicroseconds(10);
        
        digitalWrite(5, HIGH);
        delayMicroseconds(10);
        
        SPI.endTransaction();
        xSemaphoreGive(spiMutex);
        
        result = ((unsigned int)msb << 8) | (unsigned int)lsb;
        
    }
    
    return result;
}

unsigned long ADE7758::read24bits(char reg) {
    unsigned long result = 0;
    
    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
        
        digitalWrite(5, LOW);
        delayMicroseconds(10);
        
        SPI.transfer(reg & 0x7F);
        delayMicroseconds(10);  // Așteaptă, dar fără clock-uri
        
        // 🔥 DIRECT 24 clock-uri pentru date
        uint8_t b23_16 = SPI.transfer(0x00);  // clock 8-15: biții 23-16
        delayMicroseconds(5);
        
        uint8_t b15_8 = SPI.transfer(0x00);   // clock 16-23: biții 15-8
        delayMicroseconds(5);
        
        uint8_t b7_0 = SPI.transfer(0x00);    // clock 24-31: biții 7-0
        delayMicroseconds(10);
        
        digitalWrite(5, HIGH);
        delayMicroseconds(10);
        
        SPI.endTransaction();
        xSemaphoreGive(spiMutex);
        
        result = ((unsigned long)b23_16 << 16) | 
                 ((unsigned long)b15_8 << 8) | 
                 (unsigned long)b7_0;
        
    }
    
    return result;
}

int32_t ADE7758::readSigned24bits(char reg) {
    unsigned long raw = read24bits(reg);
    
    // Convertire 24-bit signed la 32-bit signed
    if (raw & 0x800000) {
        // Număr negativ - extinde semnul
        return (int32_t)(raw | 0xFF000000);
    } else {
        // Număr pozitiv
        return (int32_t)raw;
    }
}

// ==================== FUNCȚII DE SCRIERE ====================

void ADE7758::write8bits(uint8_t reg, uint8_t val) {
    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500))) {
        SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE1));

        enable();
        delayMicroseconds(5);

        SPI.transfer(reg | 0x80);   // WRITE
        delayMicroseconds(5);

        SPI.transfer(val);

        // CRITIC: hold înainte de CS HIGH
        delayMicroseconds(5);

        disable();
        delayMicroseconds(5);  // ✅ ADĂUGAT - timp pentru procesare ADE

        SPI.endTransaction();
        xSemaphoreGive(spiMutex);
    }
}

void ADE7758::write16bits(uint8_t reg, uint16_t val) {
    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500))) {
        SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE1));

        enable();
        delayMicroseconds(5);

        SPI.transfer(reg | 0x80);           // WRITE
        delayMicroseconds(5);

        SPI.transfer((uint8_t)(val >> 8));  // MSB
        delayMicroseconds(5);

        SPI.transfer((uint8_t)(val & 0xFF)); // LSB

        // CRITIC: hold înainte de CS HIGH
        delayMicroseconds(5);

        disable();
        delayMicroseconds(5);  // ✅ ADĂUGAT - timp pentru procesare ADE

        SPI.endTransaction();
        xSemaphoreGive(spiMutex);
    }
}

void ADE7758::write24bits(uint8_t reg, uint32_t val) {
    if (spiMutex && xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500))) {
        SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE1));

        enable();
        delayMicroseconds(5);

        SPI.transfer(reg | 0x80);                    // WRITE
        delayMicroseconds(5);
        SPI.transfer((uint8_t)((val >> 16) & 0xFF)); // byte 23-16
        delayMicroseconds(5);
        SPI.transfer((uint8_t)((val >> 8) & 0xFF));  // byte 15-8
        delayMicroseconds(5);
        SPI.transfer((uint8_t)(val & 0xFF));          // byte 7-0
        delayMicroseconds(5);

        disable();
        delayMicroseconds(5);

        SPI.endTransaction();
        xSemaphoreGive(spiMutex);
    }
}

void ADE7758::setupDivs(unsigned char Watt_div, unsigned char VAR_div, unsigned char VA_div) {
    write8bits(WDIV, Watt_div);
    write8bits(VARDIV, VAR_div);
    write8bits(VADIV, VA_div);
}

// ==================== FUNCȚII DE CONFIGURARE REGISTRE ====================

void ADE7758::setLcycMode(char m){
    write8bits(LCYCMODE, m);
}

int ADE7758::getLcycMode(){
    return read8bits(LCYCMODE);
}

void ADE7758::gainSetup(char integrator, char scale, char PGA2, char PGA1){
    char pgas = (integrator<<7) | (PGA2<<5) | (scale<<3) | (PGA1);
    write8bits(GAIN, pgas);  // write GAIN register, format is |3 bits PGA2 gain|2 bits full scale|3 bits PGA1 gain
}

void ADE7758::setOpMode(char m){
    write8bits(OPMODE, (unsigned char)m);
}

int ADE7758::getOpMode(){
    return (int)read8bits(OPMODE);
}

void ADE7758::setMMode(char m){
    write8bits(MMODE, (unsigned char)m);
}

int ADE7758::getMMode(){
    return (int)read8bits(MMODE);
}

unsigned long ADE7758::resetStatus(void){
    return read24bits(RSTATUS);
}

// ==================== FUNCȚII DE CITIRE MĂSURĂTORI ====================

long ADE7758::getVRMS(char phase){
  const int N = 3;
  unsigned long acc = 0;

  uint8_t reg;
  if      (phase == 0) reg = AVRMS;
  else if (phase == 1) reg = BVRMS;
  else if (phase == 2) reg = CVRMS;
  else return -1;

  for(int i = 0; i < N; i++){
    acc += (unsigned long)read24bits(reg);
    //delay(5); // mică pauză pentru stabilizare / eșantionare
  }

  return (long)(acc / N);
}

long getVRMS_raw(char phase);  // o singură citire
long getIRMS_raw(char phase);

  
long ADE7758::getVRMS_raw(char phase) {
    uint8_t reg;
    if (phase == 0) reg = AVRMS;
    else if (phase == 1) reg = BVRMS;
    else if (phase == 2) reg = CVRMS;
    else return -1;
    
    return (long)read24bits(reg);  // o singură citire, fără delay
}

long ADE7758::getIRMS_raw(char phase) {
    uint8_t reg;
    if (phase == 0) reg = AIRMS;
    else if (phase == 1) reg = BIRMS;
    else if (phase == 2) reg = CIRMS;
    else return -1;
    
    return (long)read24bits(reg);
}

long ADE7758::getIRMS(char phase){
  const int N = 3;
  unsigned long acc = 0;

  uint8_t reg;
  if      (phase == 0) reg = AIRMS;
  else if (phase == 1) reg = BIRMS;
  else if (phase == 2) reg = CIRMS;
  else return -1;

  for(int i = 0; i < N; i++){
    acc += (unsigned long)read24bits(reg);
    delay(5);
  }

  return (long)(acc / N);
}


int32_t ADE7758::getWatt(char phase) {
    switch (phase) {
        case 0: return (int32_t)(int16_t)read16bits(AWATTHR);
        case 1: return (int32_t)(int16_t)read16bits(BWATTHR);
        case 2: return (int32_t)(int16_t)read16bits(CWATTHR);
        default: return 0;
    }
}

int32_t ADE7758::getVA(char phase) {
    switch (phase) {
        case 0: return (int32_t)(int16_t)read16bits(AVAHR);
        case 1: return (int32_t)(int16_t)read16bits(BVAHR);
        case 2: return (int32_t)(int16_t)read16bits(CVAHR);
        default: return 0;
    }
}

// Nu uita și getVAR dacă există:
int32_t ADE7758::getVAR(char phase) {
    switch (phase) {
        case 0: return (int32_t)(int16_t)read16bits(AVARHR);
        case 1: return (int32_t)(int16_t)read16bits(BVARHR);
        case 2: return (int32_t)(int16_t)read16bits(CVARHR);
        default: return 0;
    }
}

int ADE7758::getFreq(void){
    return read16bits(FREQ);
}