//
// nisejjy - fake JJY (standarad time radio broadcast) station
//     using software radio on ESP32.
//
// (c) 2021 by taroh (sasaki.taroh@gmail.com)
//
// 2021. 10. 28-29: ver. 0.1: worked on JJY 40KHz
// 2021. 10. 30: ver 0.2: added Bluetooth command
// 2021. 10. 30-11. 2: ver 1.0: added codes for WWVB/DCF77/HBG/MSF/BPC
//   => removed HBG, added BSF, checked WWVB/DCF77/MSF by world radio clock.
//   * DCF77, MSF forwards 1 minute.
// 2022.  1. 21: ver 1.1: added NTP feature
//
// JJY (Japan): https://ja.wikipedia.org/wiki/JJY
// WWVB (US): https://en.wikipedia.org/wiki/WWVB
//            *set UT for WWVB.
// DCF77 (Germany) : https://www.eecis.udel.edu/~mills/ntp/dcf77.html
// (HBG (Switzerland) discon: https://msys.ch/decoding-time-signal-stations )
// BSF (Taiwan): https://en.wikipedia.org/wiki/BSF_(time_service)
// MSF (UK): https://en.wikipedia.org/wiki/Time_from_NPL_(MSF)
// BPC (China): https://harmonyos.51cto.com/posts/1731
//
// note: BSF, BPC codes are not certified.

//...................................................................
// hardware config
#define PIN_RADIO  (26) //(23)
#define PIN_BUZZ   (27) //(16)
#define PIN_LED    (25) //(22)
// note: {pin23 -> 330ohm -> 30cm loop antenna -> GND} works
//     (33mW, but detuned length (super shorten), only very weak radiowave emitted).

#include <WiFi.h>
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#define DEVICENAME "niseJJY"
//char ssid[] = "rmtether";
//char passwd[] = "tsunagasete";
char ssid[] = "earthlink";
char passwd[] = "mieiamici";
//char ssid[] = "802elecom_2.4GHz";
//char passwd[] = "DISCCSLAP";
#define TZ (9 * 60 * 60) /*JST*/

//...................................................................
// station specs
//
#define SN_JJY_E  (0) // JJY Fukushima Japan
#define SN_JJY_W  (1) // JJY Fukuoka Japan
#define SN_WWVB (2)   // WWVB US
#define SN_DCF77  (3) // DCF77 Germany
#define SN_BSF  (4)   // BSF Taiwan
#define SN_MSF  (5)   // MSF UK
#define SN_BPC  (6)   // BPC China

#define SN_DEFAULT  (SN_JJY_E)

int st_cycle2[] = { // interrupt cycle, KHz: double of station freq
  80,  // 40KHz JJY-E
  120, // 60KHz JJY-W
  120, // 60KHz WWVB
  155, // 77.5KHz DCF77
  155, // 77.5KHz BSF
  120, // 60KHz MSF
  137  // 68.5KHz BPC
};

// interrupt cycle to makeup radio wave, buzzer (500Hz = 1KHz cycle):
// peripheral freq == 80MHz
//    ex. radio freq 40KHz: intr 80KHz: 80KHz / 80MHz => 1/1000 (1/tm0cycle)
//    buzz cycle: 1KHz / 80KHz 1/80 (1/radiodiv)
int tm0cycle;
#define TM0RES    (1)
int radiodiv;

// TM0RES (interrupt counter), AMPDIV (buzz cycle(1000) / subsec(10)), SSECDIV (subsec / sec)
// don't depend on station specs. 
#define AMPDIV   (100)  // 1KHz / 100 => 10Hz, amplitude may change every 0.1 seconds
#define SSECDIV    (10) // 10Hz / 10 => 1Hz, clock ticks

// enum symbols
#define SP_0  (0)
#define SP_1  (1)
#define SP_M  (2)
#define SP_P0 (SP_1) // for MSF
#define SP_P1 (3)    // for MSF
/*#define SP_M0 (3) // for HBG
#define SP_M00 (4) // for HBG
#define SP_M000 (5) // for HBG
 */
#define SP_2  (2) // for BSF/BPC
#define SP_3  (3) // for BSF/BPC
#define SP_M4 (4) // for BSF/BPC
#define SP_MAX  (SP_M4)
//
// bits_STATION[] => *bits60: 60 second symbol buffers, initialized with patterns
// sp_STATION[] => *secpattern: 0.1sec term pattern in one second, for each symbol
// * note: when sp_STATION[n * 10], secpattern[] is like 2-dim array secpattern[n][10].
//
// JJY & WWVB  *note: comment is the format of JJY.
int8_t bits_jjy[] = {  // 60bit transmitted frame, of {SP_0, SP_1, SP_M}
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // (M), MIN10[3], 0, MIN1[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, 0, HOUR10[2], 0, HOUR1[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, 0, DOY100[2], DOY10[4], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // DOY1[4], 0, 0, PA1, PA2, 0, (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M, // 0, YEAR[8], (M)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M  // DOW[3], LS1, LS2, 0, 0, 0, 0, (M)
};
// *note: if summer time, set bit 57/58 (WWVB) (bit 38/40 (JJY, in future))
int8_t sp_jjy[] = { // in (0, 1), [SP_x][amplitude_for_0.1sec_term_in_second]
  1, 1, 1, 1, 1, 1, 1, 1, 0, 0,   // SP_0
  1, 1, 1, 1, 1, 0, 0, 0, 0, 0,   // SP_1
  1, 1, 0, 0, 0, 0, 0, 0, 0, 0    // SP_M
};
int8_t sp_wwvb[] = { // in (0, 1), [SP_x][amplitude_for_0.1sec_term_in_second]
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,   // SP_1
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1    // SP_M
};

// DCF77 encoding is LSB->MSB. //HBG *note: [0] is changed depending on DCF/HBG (also min/hour).
int8_t bits_dcf[] = {
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // 0, reserved[9]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_1, SP_0, // reserved[5], 0, 0, (0, 1)(MEZ), 0
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // 1, MIN1[4], MIN10[3], P1, (1->)
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // HOUR1[4], HOUR10[2], P2, D1[4]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // D10[2], DOW[3], M1[4], M10[1]
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M  // Y1[4], Y10[4], P3, (M)
};
int8_t sp_dcf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1    // SP_M
};
/*
int8_t sp_hbg[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_M
  0, 1, 0, 1, 1, 1, 1, 1, 1, 1,   // SP_M0    // 00sec
  0, 1, 0, 1, 0, 1, 1, 1, 1, 1,   // SP_M00   // 00sec at 00min
  0, 1, 0, 1, 0, 1, 0, 1, 1, 1    // SP_M000  // 00sec at 00/12 hour 00min
};
*/

// BSF: quad encoding.
int8_t bits_bsf[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4,
  SP_1,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,  // 1, min[3], hour[2.5], P1[.5],
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_M4  // DOM[2.5], DOW[2.5], mon[2],
                                                                // year[3.5], P2[.5], 0, 0, M
};
int8_t sp_bsf[] = {
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,   // SP_1
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1,   // SP_2
  0, 0, 0, 0, 0, 0, 1, 1, 1, 1,   // SP_3
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1    // SP_M4
};

// MSF has 4 patterns.
int8_t bits_msf[] = {
  SP_M, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0,
  SP_0, SP_0, SP_0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_P0, SP_0
};
int8_t sp_msf[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1/SP_P0 (parity 0)
  0, 0, 0, 0, 0, 1, 1, 1, 1, 1,   // SP_M
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1    // SP_P1 (parity 1)
};

// BPC: quadary and has 5 patterns.
int8_t bits_bpc[] = {
  SP_M4, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // (B), P1, P2, h[2], m[3], DOW[2]
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P3, D[3], M[2], Y[3], P4
  SP_M4, SP_1, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P1: 0, 1, 2 for 00-19, -39, -59s
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P2: 0, P3: AM/PM(0/2)+par<hmDOW>
  SP_M4, SP_2, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, // P4: par<DMY> (0/1)
  SP_0,  SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0, SP_0
};
int8_t sp_bpc[] = {
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_0
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1,   // SP_1
  0, 0, 0, 1, 1, 1, 1, 1, 1, 1,   // SP_2
  0, 0, 0, 0, 1, 1, 1, 1, 1, 1,   // SP_3
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0    // SP_M4
};

// func for makeup patterns
void mb_jjy(void);  // JJY-E, JJY-W
void mb_wwvb(void); // WWVB
void mb_dcf(void);  // DCF77
void mb_bsf(void);  // BSF
void mb_msf(void);  // MSF
void mb_bpc(void);  // BPC

int8_t *st_bits[] = {bits_jjy, bits_jjy, bits_jjy, bits_dcf, bits_bsf, bits_msf, bits_bpc};
int8_t *bits60;
int8_t *st_sp[]   = {sp_jjy, sp_jjy, sp_wwvb, sp_dcf, sp_bsf, sp_msf, sp_bpc};
int8_t *secpattern;
void (*st_makebits[])(void) = {mb_jjy, mb_jjy, mb_wwvb, mb_dcf, mb_bsf, mb_msf, mb_bpc};
void (*makebitpattern)(void);

//...................................................................
// globals
hw_timer_t *tm0 = NULL;
volatile SemaphoreHandle_t  timerSemaphore;
portMUX_TYPE  timerMux = portMUX_INITIALIZER_UNLOCKED; 
volatile uint32_t buzzup = 0;     // inc if buzz cycle (/2) passed
int istimerstarted = 0;

int radioc = 0; // 0..(RADIODIV - 1)
int ampc = 0;   // 0..(AMPDIV - 1)
int tssec = 0;  // 0..(SSECDIV - 1)

int ntpsync = 1;
time_t now;
struct tm nowtm;
//int tsec, tmin, thour,    // initial values for date/time
//    tday, tmon, tyear; // tyear: lower 2 digits of 20xx
//int tdoy, tdow; // day of year (1-365/366), day of week (0-6)
int radioout = 0, // pin output values
    buzzout = 0;
int ampmod;     // 1 if radio out is active (vibrating), 0 if reducted,
                // at cuttent subsecond-second frame for current date-time
int buzzsw = 1; // sound on/off

// extern 
void IRAM_ATTR onTimer(void);
void setup(void);
void loop(void);
void starttimer(void);
void stoptimer(void);
void ampchange(void);
void setstation(int station);
void binarize(int v, int pos, int len);
void bcdize(int v, int pos, int len);
void rbinarize(int v, int pos, int len);
void rbcdize(int v, int pos, int len);
void quadize(int v, int pos, int len);
int parity(int pos, int len);
int qparity(int pos, int len);
void setlocaltime(void);
void getlocaltime(void);
//void setdoydow(void);
//int julian(int y, int m, int d);
//void incday(void); // for DCF77
int docmd(char *buf);
int a2toi(char *chp);
void printbits60(void);
void ntpstart(void);
void ntpstop(void);

//...................................................................
// intr handler:
//   this routine is called once every 1/2f sec (where f is radio freq).
//   - reverse radio output pin if modulation flag "ampmod" == 1,
//   - count up "radioc", if exceeds "radiodev" then
//     turn on buzzer flag "buzzup"; the "buzzup" is set once in 1/1000sec
//     on every frequency of the station (so "radiodev" should be set
//     propery depending to the intr cycle "tm0cycle" of the station).
void IRAM_ATTR onTimer(void)
{
  if (! radioout && ampmod) {
    radioout = 1;
    digitalWrite(PIN_RADIO, HIGH);
  } else {
    radioout = 0;
    digitalWrite(PIN_RADIO, LOW);
  }
  radioc++;
  if (radiodiv <= radioc) {
    radioc = 0;
    portENTER_CRITICAL_ISR(&timerMux);           // CRITICAL SECTION ---
    buzzup++;
    portEXIT_CRITICAL_ISR(&timerMux);            // --- CRITICAL SECTION
    xSemaphoreGiveFromISR(timerSemaphore, NULL); // free semaphore
  }
  return;
}

//...................................................................
void setup(void)
{
  uint8_t macBT[6];

  Serial.begin(115200);
  delay(100);
  Serial.print("started...\n");

  esp_read_mac(macBT, ESP_MAC_BT);
  Serial.printf(
    "Bluetooth %s %02X:%02X:%02X:%02X:%02X:%02X...",
    DEVICENAME, macBT[0], macBT[1], macBT[2], macBT[3], macBT[4], macBT[5]);
  while (! SerialBT.begin(DEVICENAME)) {
    Serial.println("error initializing Bluetooth");
    delay(2000);
  }
  Serial.print("\n");

  if (ntpsync) {
    ntpstart();
  }
  pinMode(PIN_RADIO, OUTPUT);
  digitalWrite(PIN_RADIO, radioout);
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, buzzout);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
//  setdoydow();
  setstation(SN_DEFAULT); // timer starts
  ampchange();
  Serial.print("radio started.\n");
}


void loop() {
  int buzzup2 = 0;    // copy of buzzup: to make critical section shorter
  static char buf[128];
  static int  bufp = 0;

  if (xSemaphoreTake(timerSemaphore, 0) == pdTRUE) { // get semaphre
    portENTER_CRITICAL(&timerMux);              // CRITICAL SECTION ---
    if (buzzup) {
      buzzup2++;
      buzzup--;
    }
    portEXIT_CRITICAL(&timerMux);               // --- CRITICAL SECTION
  }
  if (buzzup2) {
    buzzup2--;
    if (! buzzout && ampmod && buzzsw) {
      buzzout = 1;
      digitalWrite(PIN_BUZZ, HIGH);
    } else {
      buzzout = 0;
      digitalWrite(PIN_BUZZ, LOW);
    }
    ampc++;
    if (AMPDIV <= ampc) {
      ampc = 0;
      tssec++;
      if (SSECDIV <= tssec) { // 1 second action --v
        tssec = 0;
        int lastmin = nowtm.tm_min;
        getlocaltime();
//          tsec++;
//          if (60 <= tsec) {
//            tsec = 0;
//            tmin++;
//            if (60 <= tmin) {
//              tmin = 0;
//              thour++;
//              if (24 <= thour) {
//                thour = 0;
//                incday();
//              }
//            }
//          }
        if (lastmin != nowtm.tm_min) {
          makebitpattern();
          printbits60();
        }
        Serial.printf("%d-%d-%d, %d(%d) %02d:%02d:%02d\n",
          nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday,
          nowtm.tm_yday, nowtm.tm_wday,
          nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec);
      }
      ampchange();
    }
  }

  while (SerialBT.available()) {
    buf[bufp] = SerialBT.read();
    if (buf[bufp] == '\n' || buf[bufp] == '\r' ||
       bufp == sizeof(buf) - 1) {
      buf[bufp] = '\0';
      docmd(buf);
      bufp = 0;
    } else {
      bufp++;
    }
  }
  delay(1); // feed watchdog
}


//...................................................................
void
starttimer(void)
{
  if (istimerstarted) {
    stoptimer();
  }
  ampc = 0;
  radioc = 0;
  timerSemaphore = xSemaphoreCreateBinary();    // create semaphore
  tm0 = timerBegin(0, tm0cycle, true);          // tm0 prescaler
  timerAttachInterrupt(tm0, &onTimer, true);    // tm0 intr routine: onTimer()
  timerAlarmWrite(tm0, TM0RES, true);           // tm0 resolution
  timerAlarmEnable(tm0);                        // tm0 start
  Serial.print("(re)started timer...\n");
  istimerstarted = 1;
  return;
}

void
stoptimer(void)
{
  if (istimerstarted) {
    timerEnd(tm0);
    istimerstarted = 0;
  }
  return;
}

// setup amplitude value ampmod depends on bit pattern & 0.1 second frame
void
ampchange(void)
{
  ampmod = secpattern[bits60[nowtm.tm_sec] * 10 + tssec];
  if (ampmod) {
    digitalWrite(PIN_LED, HIGH);
    Serial.print("~");
  } else {
    digitalWrite(PIN_LED, LOW);
    Serial.print(".");
  }
  return;
}


void
setstation(int station)
{
  stoptimer();
  Serial.printf("station #%d:\n", station);
  tm0cycle = 80000 / st_cycle2[station];
  radiodiv = st_cycle2[station];
  Serial.printf("  freq %fMHz, timer intr: 80M / (%d x %d), buzz/radio: /%d\n",
    (float)radiodiv / 2., tm0cycle, TM0RES, radiodiv);
  bits60 = st_bits[station];
  Serial.printf("  bits60 pattern: ");
  for (int i = 0; i < 60; i++) {
    Serial.printf("%d", (int)bits60[i]);
  }
  secpattern = st_sp[station];
  Serial.printf("\n  second pattern: ");
  for (int i = 0; i < 10; i++) {
    Serial.printf("%d", (int)secpattern[i]);
  }
  Serial.printf("...\n");
  makebitpattern = st_makebits[station];
  makebitpattern();
  printbits60();
  starttimer();
  return;
}

//...................................................................
// makeup bit pattern for current date, hour:min

void
mbc_wwvbjjy(void) //--- [0..33] are common in WWVB/JJY
{
  binarize(nowtm.tm_min / 10, 1, 3);
  binarize(nowtm.tm_min % 10, 5, 4);
  binarize(nowtm.tm_hour / 10, 12, 2);
  binarize(nowtm.tm_hour % 10, 15, 4);
  int y100 = nowtm.tm_yday / 100;
  int y1 = (nowtm.tm_yday - y100 * 100);
  int y10 = y1 / 10;
  y1 = y1 % 10;
  binarize(y100, 22, 2);
  binarize(y10, 25, 4);
  binarize(y1, 30, 4);
//  Serial.printf("min%d-%d hour%d-%d doy%d-%d-%d ", tmin / 10, tmin % 10, thour / 10, thour % 10,
//    y100, y10, y1);
  return;  
}

void
mb_jjy(void)   //---- JJY_E & JJY_W
{
  Serial.print("encode JJY format - ");
  mbc_wwvbjjy();
  bits60[36] = parity(12, 7);
  bits60[37] = parity(1, 8);
//  Serial.printf("pa2%d ", s % 2);
  binarize((nowtm.tm_year - 100) / 10, 41, 4);
  binarize(nowtm.tm_year % 10, 45, 4);
  binarize(nowtm.tm_wday, 50, 3);
//  Serial.printf("year%d-%d dow%d\n", tyear / 10, tyear % 10, tdow);
  return;
}

void
mb_wwvb(void)
{
  Serial.print("encode WWVB format - ");
  mbc_wwvbjjy();
  binarize((nowtm.tm_year - 100) / 10, 45, 4);
  binarize(nowtm.tm_year % 10, 50, 4);
  return;
}

void
mb_dcf(void)  //---- DCF77
{
  Serial.print("encode DCF77 format - ");
//  bits60[0] = SP_0; // (obsolate) this routine is used also by mb_hbg() which changes bits60[0]
  rbcdize(nowtm.tm_min, 21, 7);
  bits60[28] = parity(21, 7);
  rbcdize(nowtm.tm_hour, 29, 6);
  bits60[35] = parity(29, 6);
  rbcdize(nowtm.tm_mday, 36, 6);
  rbinarize(nowtm.tm_wday, 42, 3);
  rbcdize(nowtm.tm_mon + 1, 45, 5);
  rbcdize(nowtm.tm_year - 100, 50, 8);
  bits60[58] = parity(36, 22);
  return;
}

/*
void
mb_hbg(void)   //---- HBG
{
    mb_dcf();
    if (tmin != 0) {
      bits60[0] = SP_M0;
    } else {
      if (thour % 12 != 0) {
        bits60[0] = SP_M00;
      } else {
        bits60[0] = SP_M000;
      }
    }
    return;
}
*/

void
mb_bsf(void)   //---- BSF
{
  Serial.print("encode BSF format - ");
  quadize(nowtm.tm_min, 41, 3);
  quadize(nowtm.tm_hour * 2, 44, 3);
  bits60[46] |= qparity(41, 6);
  quadize(nowtm.tm_mday * 2, 47, 3);
  bits60[49] |= nowtm.tm_wday / 4;
  quadize(nowtm.tm_wday % 4, 50, 1);
  quadize(nowtm.tm_mon + 1, 51, 2);
  quadize((nowtm.tm_year - 100) * 2, 53, 4);
  bits60[56] |= qparity(47, 10);
  return;
}

void
mb_msf(void)
{
  Serial.print("encode MSF format - ");
  bcdize(nowtm.tm_year - 100, 17, 8);
  bcdize(nowtm.tm_mon + 1, 25, 5);
  bcdize(nowtm.tm_mday, 30, 6);
  binarize(nowtm.tm_wday, 36, 3);
  bcdize(nowtm.tm_hour, 39, 6);
  bcdize(nowtm.tm_min, 45, 7);
  bits60[54] = parity(17, 8) * 2 + 1;  // in MSF parity bits, values are {1, 3} (SP_1, SP_P1)
  bits60[55] = parity(25, 11) * 2 + 1; // for parity {0, 1}.
  bits60[56] = parity(36, 3) * 2 + 1;
  bits60[57] = parity(39, 13) * 2 + 1;
  bits60[58] = SP_1;  // change here to SP_P1 if summertime 

  return;
}

void
mb_bpc(void)
{
  Serial.print("encode BPC format - ");
  quadize(nowtm.tm_hour % 12, 3, 2);
  quadize(nowtm.tm_min, 5, 3);
  quadize(nowtm.tm_wday, 8, 2);
  bits60[10] = (nowtm.tm_hour / 12) * 2 + qparity(3, 7);
  quadize(nowtm.tm_mday, 11, 3);
  quadize(nowtm.tm_mon + 1, 14, 2);
  quadize(nowtm.tm_year - 100, 16, 3);
  bits60[19] = qparity(11, 8);
  for (int i = 2; i < 20; i++) {
    bits60[20 + i] = bits60[i];
    bits60[40 + i] = bits60[i];
  }
  return;
}


// write binary value into bit pattern (little endian)
void
binarize(int v, int pos, int len)
{
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    bits60[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// continuous (over 4 bit) BCD to write
void
bcdize(int v, int pos, int len)
{
  int l;

  pos = pos + len - 1;
  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    binarize(v % 10, pos - l + 1, l);
    v = v / 10;
    pos = pos - l;
    len = len - l;
  }
  return;
}

// LSB->MSB (big endian) binarize
void
rbinarize(int v, int pos, int len)
{
  for ( ; 0 < len; pos++, len--) {
    bits60[pos] = (uint8_t)(v & 1);
    v >>= 1;
  }
  return;
}

// LSB->MSB BCDize
void
rbcdize(int v, int pos, int len)
{
  int l;

Serial.printf("\nrbcd %d[pos %d, len%d]=", v, pos, len);
  while (0 < len) {
    if (4 <= len) {
      l = 4;
    } else {
      l = len;
    }
    rbinarize(v % 10, pos, l);
    v = v / 10;
    pos = pos + l;
    len = len - l;
  }
  return;
}


// 4-ary encoding (little endian)
void
quadize(int v, int pos, int len)
{
  for (pos = pos + len - 1; 0 < len; pos--, len--) {
    bits60[pos] = (uint8_t)(v & 3);
    v >>= 2;
  }
  return;
}

// calculate even parity
int
parity(int pos, int len)
{
  int s = 0;
  
  for (pos; 0 < len; pos++, len--) {
    s += bits60[pos];
  }
  return (s % 2);
}

// binary parity for 4-ary data (for BSF/BPC): is it OK?
int
qparity(int pos, int len)
{
  int s = 0;
  
  for (pos; 0 < len; pos++, len--) {
    s += (bits60[pos] & 1) + ((bits60[pos] & 2) >> 1);
  }
  return (s % 2);
}


//// calculate doy (day of year)/dow (day of week) from YY/MM/DD
//void
//setdoydow(void)
//{
//  int j0 = julian(tyear, 1, 1);      // new year day of this year
//  int j1 = julian(tyear, tmon, tday);
//  tdoy = j1 - j0 + 1; // 1..365/366
//  tdow = j1 % 7;      // 0..6
//  return;
//}
//
//// return julian date (? relative date from a day)
//// sunday is multiple of 7
//int
//julian(int y, int m, int d)
//{
//  if (m <= 2) {
//    m = m + 12;
//    y--;
//  }
//  return y * 1461 / 4 + (m + 1) * 153 / 5 + d + 6;
////  1461 / 4 == 365.25, 153 / 5 == 30.6
//}
//
//// increment tday-tmon-tyear, tdoy, tdow
//void
//incday(void)
//{
//  int year1 = tyear;   // year of next month
//  int mon1 = tmon + 1; // next month
//  if (12 < mon1) {
//    mon1 = 1;
//    year1++;
//  }
//  int day1 = tday + 1; // date# of tomorrow
//  if (julian(year1, mon1, 1) - julian(tyear, tmon, 1) < day1) {
//    tday = 1;  // date# exceeds # of date in this month
//    tmon = mon1;
//    tyear = year1;
//  } else {
//    tday = day1;
//  }
//  setdoydow(); // tdoy, tdow is updated from tyear-tmonth-tday
//  return;
//}

//...................................................................
// Bluetooth command
//
// y[01]: NTP sync off/on
//   to force set the current date/time (d/t), first turn off NTP sync.
// dYYMMDD: set date to YY/MM/DD
// tHHmmSS: set time to HH:mm:SS
// z[01]: buzzer off/on
// s[jkwdhmb]: set station to JJY_E, JJY_W, WWVB, DCF77, HBG, MSF, BPC

int
docmd(char *buf)
{
  int arg1, arg2;
  Serial.printf("cmd: >>%s<<\n", buf);
  if (buf[0] == 'd' || buf[0] == 'D') { // set date
    if (strlen(buf) != 7) {
      return 0;
    }
    int y = a2toi(buf + 1);
    int m = a2toi(buf + 3);
    int d = a2toi(buf + 5);
    Serial.printf("%d %d %d\n", y, m, d);
    if (y < 0 || m < 0 || 12 < m || d < 0 || 31 < d) {  // can set Feb 31 :-)
      return 0;
    }
    nowtm.tm_year = y + 100;
    nowtm.tm_mon = m - 1;
    nowtm.tm_mday = d;
    setlocaltime();
    Serial.printf("set date: >>%s<<\n", buf + 1);
    return 1;
  } else if (buf[0] == 't' || buf[0] == 'T') { // set time & start tick
    if (strlen(buf) != 7) {
      return 0;
    }
    int h = a2toi(buf + 1);
    int m = a2toi(buf + 3);
    int s = a2toi(buf + 5);
    if (h < 0 || 24 < h || m < 0 || 60 < m || s < 0 || 60 < s) {
      return 0;
    }
    nowtm.tm_hour = h;
    nowtm.tm_min = m;
    nowtm.tm_sec = s;
    tssec = 0;
    ampc = 0;
    radioc = 0; // no semaphore lock: don't care if override by intr routine :-)
    setlocaltime();
    Serial.printf("set time...restart tick: >>%s<<\n", buf + 1);
    return 1;
  } else if (buf[0] == 'z' || buf[0] == 'Z') { // buzzer on(1)/off(0)
    if (buf[1] == '0') {
      buzzsw = 0;
    } else if (buf[1] == '1') {
      buzzsw = 1;
    } else {
      return 0;
    }
    Serial.printf("buzzer: >>%c<<\n", buf + 1);
    return 1;
  } else if (buf[0] == 's' || buf[0] == 'S') { // set station
    char s[] = //"jJkKwWdDhHmMbB"
              {'j', 'J', 'k', 'K', 'w', 'W', 'd', 'D', 't', 'T', 'm', 'M', 'c', 'C', '\0'},
              *chp;
    if ((chp = strchr(s, buf[1])) != NULL) {
      setstation((int)(chp - s) / 2);
      return 1;
    } else {
      return 0;
    }
  } else if (buf[0] == 'y' || buf[0] == 'Y') { // NTP sync
        if (buf[1] == '0') {
      ntpsync = 0;
      ntpstop();
    } else if (buf[1] == '1') {
      ntpsync = 1;
      ntpstart();
    } else {
      return 0;
    }
  }
  return 0;
}

int
a2toi(char *chp)
{
  int v = 0;
  for (int i = 0; i < 2; chp++, i++) {
    if (*chp < '0' || '9' < *chp) {
      return -1;
    }
    v = v * 10 + (*chp - '0');
  }
  return v;
}


void
printbits60(void)
{
  Serial.print("\n");
  for (int i = 0; i < 60; i++) {
    Serial.print(bits60[i]);
  }
  Serial.print("\n");
  return;
}


void
ntpstart(void)
{
  int i;

// WiFi, NTP setup
  Serial.print("Attempting to connect to Network named: ");
  Serial.println(ssid);                   // print the network name (SSID);
  WiFi.begin(ssid, passwd);
  for (i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
    Serial.print(".");
    delay(1000);
  }
  if (i == 10) {
    ntpsync = 0;
    return;
  }
  IPAddress ip = WiFi.localIP();
  Serial.printf("IP Address: ");
  Serial.println(ip);
  Serial.printf("configureing NTP...");
  configTime(TZ, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp"); // enable NTP
  for (int i = 0; i < 10 && ! getLocalTime(&nowtm); i++) {
    Serial.printf(".");
    delay(1000);
  }
  if (i == 10) {
    ntpsync = 0;
    return;
  }
  Serial.printf("done\n");
}


void
ntpstop(void)
{
  ntpsync = 0;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}


void
setlocaltime(void)
{
  time_t nowtime = mktime(&nowtm) /*+ TZ */;
  struct timeval tv = {
    .tv_sec = nowtime
  };
  settimeofday(&tv, NULL);
  getlocaltime(); // to make wday/yday
}


void
getlocaltime(void)
{
  if (ntpsync) {
    getLocalTime(&nowtm);
  } else {
    time_t nowtime;
    time(&nowtime);
    struct tm *ntm;
    ntm = localtime(&nowtime);
    nowtm = *ntm;
  }
}
