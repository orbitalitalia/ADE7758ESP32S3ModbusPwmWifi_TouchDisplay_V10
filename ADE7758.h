//voor als de library per ongeluk twee keer wordt geimporteerd
#ifndef ADE7758_h
#define ADE7758_h
#include "Arduino.h"    

#define WRITE 0x80 //0x80 = b10000000 pentru scriere BT7 trebuie sÄƒ fie 1

// ==================== REGISTRE ADE7758 ====================
#define AWATTHR    0x01 //---------16
#define BWATTHR    0x02 //---------16
#define CWATTHR    0x03 //---------16

#define AVARHR     0x04 //---------16
#define BVARHR     0x05 //---------16
#define CVARHR     0x06 //---------16

#define AVAHR      0x07 //---------16
#define BVAHR      0x08 //---------16
#define CVAHR      0x09 //---------16

#define AIRMS      0x0A //---------24
#define BIRMS      0x0B //---------24
#define CIRMS      0x0C //---------24
#define AVRMS      0x0D //---------24
#define BVRMS      0x0E //---------24
#define CVRMS      0x0F //---------24

#define FREQ       0x10 //---------12
#define TEMP       0x11 //---------8
#define WFORM      0x12 //---------24
#define OPMODE     0x13 //---------8
#define MMODE      0x14 //---------8
#define WAVMODE    0x15 //---------8
#define COMPMODE   0x16 //---------8
#define LCYCMODE   0x17 //---------8
#define MASK       0x18 //---------24
#define STATUS     0x19 //---------24
#define RSTATUS    0x1A //---------24
#define ZXTOUT     0x1B //---------16
#define LINECYC    0x1C //---------16
#define SAGCYC     0x1D //---------8
#define SAGLVL     0x1E //---------8
#define VPINTLVL   0x1F //---------8
#define IPINTLVL   0x20 //---------8
#define VPEAK      0x21 //---------8
#define IPEAK      0x22 //---------8
#define GAIN       0x23 //---------8
#define AVRMSGAIN  0x24 //---------12
#define BVRMSGAIN  0x25 //---------12
#define CVRMSGAIN  0x26 //---------12
#define AIGAIN     0x27 //---------12
#define BIGAIN     0x28 //---------12
#define CIGAIN     0x29 //---------12
#define AWG        0x2A //---------12
#define BWG        0x2B //---------12
#define CWG        0x2C //---------12
#define AVARG      0x2D //---------12
#define BVARG      0x2E //---------12
#define CVARG      0x2F //---------12
#define AVAG       0x30 //---------12
#define BVAG       0x31 //---------12
#define CVAG       0x32 //---------12
#define AVRMSOS    0x33 //---------12
#define BVRMSOS    0x34 //---------12
#define CVRMSOS    0x35 //---------12
#define AIRMSOS    0x36 //---------12
#define BIRMSOS    0x37 //---------12
#define CIRMSOS    0x38 //---------12
#define AWATTOS    0x39 //---------12
#define BWATTOS    0x3A //---------12
#define CWATTOS    0x3B //---------12
#define AVAROS     0x3C //---------12
#define BVAROS     0x3D //---------12
#define CVAROS     0x3E //---------12
#define APHCAL     0x3F //---------7
#define BPHCAL     0x40 //---------7
#define CPHCAL     0x41 //---------7
#define WDIV       0x42 //---------8
#define VARDIV     0x43 //---------8
#define VADIV      0x44 //---------8
#define APCFNUM    0x45 //---------16
#define APCFDEN    0x46 //---------12
#define VARCFNUM   0x47 //---------16
#define VARCFDEN   0x48 //---------12
#define CHKSUM     0x7E //---------8
#define VERSION    0x7F //---------8

// ==================== BIȚI DE CONFIGURARE ====================

/**
OPERATIONAL MODE REGISTER (0x13)
Bit Location    Bit Mnemonic    Default Value   Description
0         DISHPF        0         HPFs Ã®n toate canalele de curent sunt dezactivate cÃ¢nd acest bit este setat.
1         DISLPF        0         LPFs dupÄƒ multiplicatorii watt È™i VAR sunt dezactivate cÃ¢nd acest bit este setat.
2         DISCF         1         IeÈ™irile de frecvenÈ›Äƒ APCF È™i VARCF sunt dezactivate cÃ¢nd acest bit este setat.
3 to 5    DISMOD        0         SetÃ¢nd aceÈ™ti biÈ›i, ADC-urile ADE7758 pot fi oprite.
6         SWRST         0         Software Chip Reset. Un transfer de date cÄƒtre ADE7758 nu ar trebui sÄƒ aibÄƒ loc timp de cel puÈ›in 18 Î¼s dupÄƒ un reset software.
7         RESERVED      0         Ar trebui lÄƒsat la 0.
*/

#define DISHPF  0x01
#define DISLPF  0x02
#define DISCF   0x04
#define SWRST   0x40

#define RUN     0x18

// Faze
#define PHASE_A 0
#define PHASE_B 1
#define PHASE_C 2

// BiÈ›i pentru LCYCMODE
#define LWATT   0x01
#define LVAR    0x02
#define LVA     0x04
#define ZXSEL_A 0x08
#define ZXSEL_B 0x10
#define ZXSEL_C 0x20
#define RSTREAD 0x40
#define FREQSEL 0x80

// BiÈ›i pentru STATUS/MASK
#define AEHF      0x0001    // bit 0
#define REHF      0x0002    // bit 1
#define VAEHF     0x0004    // bit 2
#define SAGA      0x0008    // bit 3
#define SAGB      0x0010    // bit 4
#define SAGC      0x0020    // bit 5
#define ZXTOA     0x0040    // bit 6
#define ZXTOB     0x0080    // bit 7
#define ZXTOC     0x0100    // bit 8
#define ZXA       0x0200    // bit 9
#define ZXB       0x0400    // bit 10
#define ZXC       0x0800    // bit 11
#define LENERGY   0x1000    // bit 12
#define RESET     0x2000    // bit 13
#define PKV       0x4000    // bit 14
#define PKI       0x8000    // bit 15
#define WFSM      0x010000  // bit 16
#define REVPAP    0x020000  // bit 17
#define REVPRP    0x040000  // bit 18
#define SEQERR    0x080000  // bit 19

// Constante de gain
#define GAIN_1                  0x00
#define GAIN_2                  0x01
#define GAIN_4                  0x02
#define INTEGRATOR_ON           1
#define INTEGRATOR_OFF          0
#define FULLSCALESELECT_0_5V    0x00
#define FULLSCALESELECT_0_25V   0x01
#define FULLSCALESELECT_0_125V  0x02

// ==================== CLASA ADE7758 ====================

class ADE7758 {
  public:
    ADE7758();
    void Init(void);
    void setSPI(void);
    void closeSPI(void);
    
    // Functii de citire mÄƒsurÄƒtori
    long getVRMS(char phase);
    long getIRMS(char phase);
    int32_t getWatt(char phase);
    int32_t getVA(char phase);
    int32_t getVAR(char phase);   // ← adaugă această linie

    long getVRMS_raw(char phase);  // o singură citire
long getIRMS_raw(char phase);
    
    // Wrapper pentru compatibilitate (getVa cu 'a' mic)
    int32_t getVa(char phase) { return getVA(phase); }
    
    int getFreq(void);
    
    // FuncÈ›ii de scriere registre
    void write8bits(uint8_t reg, uint8_t val);
    void write16bits(uint8_t reg, uint16_t val);
    void write24bits(uint8_t reg, uint32_t val);   // ← linia asta
    void setupDivs(unsigned char Watt_div, unsigned char VAR_div, unsigned char VA_div);
    
    // Wrapper-e pentru compatibilitate cu cod vechi
    void write8(uint8_t reg, uint8_t val) { write8bits(reg, val); }
    void write16(uint8_t reg, uint16_t val) { write16bits(reg, val); }
    
    // FuncÈ›ii de configurare registre
    void setLcycMode(char m);
    int getLcycMode();
    void setOpMode(char m);
    int getOpMode();
    void setMMode(char m);
    int getMMode();
    void gainSetup(char integrator, char scale, char PGA2, char PGA1);
    unsigned long resetStatus(void);
    
    // FuncÈ›ii de citire low-level
    unsigned char read8bits(char reg);
    unsigned int read16bits(char reg);
    unsigned long read24bits(char reg);
    int32_t readSigned24bits(char reg);
    
  private:
    int CS;
    void enable();
    void disable();
    long getInterruptStatus();
    long getResetInterruptStatus();

};

#endif