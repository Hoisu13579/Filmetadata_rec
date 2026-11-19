#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <ESP32Time.h>
#include <Preferences.h>

#define SHUTTER 5
#define PIN_MODE 6
#define PIN_UP 7
#define PIN_DOWN 10
#define PIN_SDA 8
#define PIN_SCL 9

#define OLED_W 128
#define OLED_H 32
#define OLED_ADDR 0x3C

#define DEBOUNCE_MS 200
#define UI_TIMEOUT_MS 30000


#define NTAG_ADDR 0x55
#define NTAG_FIRST_BLOCK 1
#define NTAG_USER_BLOCKS 55
#define NTAG_USER_BYTES (NTAG_USER_BLOCKS * 16)   // 880
#define NFC_TEXT_MAX 860

volatile bool shutterFlag = false;
uint32_t lastShotMs = 0;
constexpr uint8_t MAX_LOG = 40;

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1, 400000UL, 100000UL);
RTC_DS3231 rtc;
ESP32Time  espClock(0);
Preferences prefs;

enum Mode : uint8_t {
  MODE_RUN,
  MODE_NFC,          
  MODE_RESET,
  MODE_SET_YEAR,
  MODE_SET_MONTH,
  MODE_SET_DAY,
  MODE_SET_HOUR,
  MODE_SET_MIN
};
Mode mode = MODE_RUN;

uint32_t frameCount = 0;
uint32_t logEpoch[MAX_LOG];

struct Button {
  volatile bool     pressed;
  volatile uint32_t lastMs;
};
Button btnMode = {false, 0};
Button btnUp   = {false, 0};
Button btnDown = {false, 0};

bool rtcOk = false;
bool nfcOk = false;
bool nfcSent = false;                 

int  editY, editMo, editD, editH, editMi;
bool editDirty = false;

uint32_t lastInputMs = 0;
char     toastMsg[12] = "";
uint32_t toastUntil = 0;
bool     needRedraw = true;

uint8_t nfcImg[NTAG_USER_BYTES];      

void IRAM_ATTR isrShutter() {
  shutterFlag = true;
}

void IRAM_ATTR handleButtonIsr(Button &b) {
  uint32_t now = millis();
  if (now - b.lastMs >= DEBOUNCE_MS) {
    b.lastMs  = now;
    b.pressed = true;
  }
}
void IRAM_ATTR isrMode(){
  handleButtonIsr(btnMode);
}
void IRAM_ATTR isrUp(){
  handleButtonIsr(btnUp);
}
void IRAM_ATTR isrDown(){ 
  handleButtonIsr(btnDown);
}

bool takePress(Button &b) {
  noInterrupts();
  bool p = b.pressed;
  b.pressed = false;
  interrupts();
  return p;
}

void saveState() {
  prefs.putUInt("count", frameCount);
  prefs.putBytes("log", logEpoch, sizeof(logEpoch));
}

void loadState() {
  frameCount = prefs.getUInt("count", 0);
  memset(logEpoch, 0, sizeof(logEpoch));
  prefs.getBytes("log", logEpoch, sizeof(logEpoch));
}

// rtc
uint32_t nowEpoch() {
  return espClock.getEpoch();
}

void formatDateTime(uint32_t epoch, char *buf, size_t len) {
  DateTime dt(epoch);
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
}

int wrapValue(int v, int lo, int hi) {
  if (v > hi) return lo;
  if (v < lo) return hi;
  return v;
}

void showToast(const char *msg, uint32_t ms) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = '\0';
  toastUntil = millis() + ms;
  needRedraw = true;
}

// ═════════════════════════════════════════════
//  NTAG I2C 읽기/쓰기
// ═════════════════════════════════════════════
bool ntagReadBlock(uint8_t block, uint8_t *buf) {
  Wire.beginTransmission(NTAG_ADDR);
  Wire.write(block);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(NTAG_ADDR, 16) != 16) return false;
  for (int i = 0; i < 16; i++) buf[i] = Wire.read();
  return true;
}

bool ntagWriteBlock(uint8_t block, const uint8_t *buf) {
  Wire.beginTransmission(NTAG_ADDR);
  Wire.write(block);
  Wire.write(buf, 16);
  if (Wire.endTransmission() != 0) return false;
  delay(5);                                    
  return true;
}


String buildLogText() {
  if (frameCount == 0) return "DB1|0";

  uint32_t first = (frameCount > MAX_LOG) ? frameCount - MAX_LOG + 1 : 1;

  char dt[24], head[48];
  formatDateTime(logEpoch[(first - 1) % MAX_LOG], dt, sizeof(dt));
  snprintf(head, sizeof(head), "DB1|%lu|%s|", (unsigned long)first, dt);
  String out = head;

  for (uint32_t n = first + 1; n <= frameCount; n++) {
    int32_t delta = (int32_t)(logEpoch[(n - 1) % MAX_LOG] - logEpoch[(n - 2) % MAX_LOG]);
    char d[14];
    snprintf(d, sizeof(d), (n == first + 1) ? "%ld" : ",%ld", (long)delta);
    if (out.length() + strlen(d) > NFC_TEXT_MAX) break; 
    out += d;
  }
  return out;
}

size_t buildNdefImage(uint8_t *img) {
  memset(img, 0, NTAG_USER_BYTES);

  String text = buildLogText();
  uint32_t textLen    = text.length();
  uint32_t payloadLen = 3 + textLen;
  uint32_t recordLen  = 7 + payloadLen;

  size_t i = 0;
  img[i++] = 0x03;                       
  if (recordLen < 0xFF) {
    img[i++] = recordLen;
  } else {
    img[i++] = 0xFF;
    img[i++] = (recordLen >> 8) & 0xFF;
    img[i++] = recordLen & 0xFF;
  }

  img[i++] = 0xC1;                       
  img[i++] = 0x01;                       
  img[i++] = (payloadLen >> 24) & 0xFF;
  img[i++] = (payloadLen >> 16) & 0xFF;
  img[i++] = (payloadLen >> 8)  & 0xFF;
  img[i++] = payloadLen & 0xFF;
  img[i++] = 'T';                        // Text

  img[i++] = 0x02;                       // 언어코드 "en", UTF-8
  img[i++] = 'e';
  img[i++] = 'n';
  memcpy(img + i, text.c_str(), textLen);
  i += textLen;

  img[i++] = 0xFE;                       // "끝"
  return i;
}


bool ntagWriteLog() {
  size_t len = buildNdefImage(nfcImg);
  uint8_t blocks = (len + 15) / 16;      
  for (uint8_t b = 0; b < blocks; b++) {
    if (!ntagWriteBlock(NTAG_FIRST_BLOCK + b, nfcImg + b * 16)) return false;
  }
  Serial.printf("[NFC] wrote %u bytes (%u blocks)\n", (unsigned)len, blocks);
  return true;
}

void sendToTag() {
  nfcSent = nfcOk && ntagWriteLog();
  needRedraw = true;
}

// ═════════════════════════════════════════════
//  셔터 / 롤
// ═════════════════════════════════════════════
void processShots() {
  if (!shutterFlag) return;
  shutterFlag = false;

  if (millis() - lastShotMs < 50) return;
  lastShotMs = millis();

  frameCount++;
  logEpoch[(frameCount - 1) % MAX_LOG] = nowEpoch();
  saveState();

  char t[12];
  snprintf(t, sizeof(t), "F%02lu", (unsigned long)frameCount);
  showToast(t, 800);
}

void resetRoll() {
  frameCount = 0;
  memset(logEpoch, 0, sizeof(logEpoch));
  saveState();
  showToast("NEW ROLL", 1200);
}

void loadEditFromNow() {
  DateTime dt(nowEpoch());
  editY = dt.year(); editMo = dt.month(); editD = dt.day();
  editH = dt.hour(); editMi = dt.minute();
  editDirty = false;
}

void adjustField(int delta) {
  switch (mode) {
    case MODE_SET_YEAR:  editY  = wrapValue(editY  + delta, 2000, 2099); break;
    case MODE_SET_MONTH: editMo = wrapValue(editMo + delta, 1, 12);      break;
    case MODE_SET_DAY:   editD  = wrapValue(editD  + delta, 1, 31);      break;
    case MODE_SET_HOUR:  editH  = wrapValue(editH  + delta, 0, 23);      break;
    case MODE_SET_MIN:   editMi = wrapValue(editMi + delta, 0, 59);      break;
    default: return;
  }
  editDirty = true;
}

void commitEdit() {
  if (!editDirty) return;
  DateTime nd(editY, editMo, editD, editH, editMi, 0);
  if (rtcOk) rtc.adjust(nd);
  espClock.setTime(nd.unixtime());
  editDirty = false;
  showToast("SAVED", 1000);
}


void onUpDown(int delta) {
  if (mode >= MODE_SET_YEAR) {
    adjustField(delta);
  } else if (mode == MODE_NFC) {
    if (delta > 0) sendToTag();          // UP = 다시 쓰기 (실패했을 때 재시도용)
  } else if (mode == MODE_RESET) {
    if (delta > 0) resetRoll();
    mode = MODE_RUN;
  }
}

void handleButtons() {
  bool m = takePress(btnMode);
  bool u = takePress(btnUp);
  bool d = takePress(btnDown);

  if (m || u || d) {
    lastInputMs = millis();
    needRedraw = true;
  }

  if (m) {
    switch (mode) {
      case MODE_RUN:       mode = MODE_NFC; sendToTag(); break;   // 들어가자마자 전송
      case MODE_NFC:       mode = MODE_RESET; break;
      case MODE_RESET:     loadEditFromNow(); mode = MODE_SET_YEAR; break;
      case MODE_SET_YEAR:  mode = MODE_SET_MONTH; break;
      case MODE_SET_MONTH: mode = MODE_SET_DAY; break;
      case MODE_SET_DAY:   mode = MODE_SET_HOUR; break;
      case MODE_SET_HOUR:  mode = MODE_SET_MIN; break;
      case MODE_SET_MIN:   commitEdit(); mode = MODE_RUN; break;
    }
  }
  if (u) onUpDown(+1);
  if (d) onUpDown(-1);
}

void checkUiTimeout() {
  if (mode != MODE_RUN && millis() - lastInputMs > UI_TIMEOUT_MS) {
    editDirty = false;
    mode = MODE_RUN;
    needRedraw = true;
  }
}

// ═════════════════════════════════════════════
//  디스플레이
// ═════════════════════════════════════════════
const char *setLabel() {
  switch (mode) {
    case MODE_SET_YEAR:  return "SET YEAR";
    case MODE_SET_MONTH: return "SET MONTH";
    case MODE_SET_DAY:   return "SET DAY";
    case MODE_SET_HOUR:  return "SET HOUR";
    case MODE_SET_MIN:   return "SET MIN";
    default:             return "";
  }
}

int setValue() {
  switch (mode) {
    case MODE_SET_YEAR:  return editY;
    case MODE_SET_MONTH: return editMo;
    case MODE_SET_DAY:   return editD;
    case MODE_SET_HOUR:  return editH;
    case MODE_SET_MIN:   return editMi;
    default:             return 0;
  }
}

void drawScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (millis() < toastUntil) {
    display.setTextSize(2);
    display.setCursor(0, 8);
    display.print(toastMsg);
    display.display();
    return;
  }

  switch (mode) {
    case MODE_RUN: {
      DateTime dt(nowEpoch());
      display.setTextSize(2);
      display.setCursor(0, 0);
      display.printf("%02d:%02d:%02d", dt.hour(), dt.minute(), dt.second());
      display.setTextSize(1);
      display.setCursor(0, 22);
      display.printf("%04d-%02d-%02d  F%02lu",
                     dt.year(), dt.month(), dt.day(), (unsigned long)frameCount);
      if (!rtcOk) { display.setCursor(104, 0); display.print("!RTC"); }
      if (!nfcOk) { display.setCursor(104, 8); display.print("!NFC"); }
      break;
    }

    case MODE_NFC:
      display.setTextSize(1);
      display.setCursor(0, 0);
      if (!nfcOk)        display.print("NFC MODULE ERROR");
      else if (nfcSent)  display.printf("SENT F%02lu  UP=AGAIN", (unsigned long)frameCount);
      else               display.print("WRITE FAIL UP=RETRY");
      if (nfcSent) {
        display.setTextSize(2);
        display.setCursor(0, 14);
        display.print("TAP PHONE");
      }
      break;

    case MODE_RESET:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("NEW ROLL? (RESET)");
      display.setCursor(0, 20);
      display.print("UP=YES   DOWN=NO");
      break;

    default:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(setLabel());
      display.setCursor(80, 0);
      display.print("UP/DN");
      display.setTextSize(2);
      display.setCursor(0, 14);
      display.printf(mode == MODE_SET_YEAR ? "%04d" : "%02d", setValue());
      break;
  }
  display.display();
}

void updateDisplay() {
  static uint32_t lastSec = 0;
  static bool lastToast = false;

  uint32_t sec = nowEpoch();
  bool toastActive = millis() < toastUntil;

  if (!needRedraw && sec == lastSec && toastActive == lastToast) return;

  drawScreen();
  lastSec = sec;
  lastToast = toastActive;
  needRedraw = false;
}

// ═════════════════════════════════════════════
//  setup / loop
// ═════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  pinMode(SHUTTER,  INPUT_PULLUP);
  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(PIN_UP,   INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    Serial.println("[OLED] init failed");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("DATA BACK BOOT...");
  display.display();

  // RTC 시간이 날아갔으면 2000-01-01 00:00:00 부터 (DS3231 최소 연도)
  DateTime startTime(2000, 1, 1, 0, 0, 0);
  uint32_t bootEpoch;
  rtcOk = rtc.begin(&Wire);
  if (rtcOk) {
    if (rtc.lostPower()) {
      Serial.println("[RTC] lost power, reset to 2000-01-01");
      rtc.adjust(startTime);
    }
    bootEpoch = rtc.now().unixtime();
  } else {
    Serial.println("[RTC] not found");
    bootEpoch = startTime.unixtime();
  }
  espClock.setTime(bootEpoch);

  prefs.begin("databack", false);
  loadState();

  // 블록 0을 읽어보는 걸로 모듈 연결 확인 (읽기만!)
  uint8_t b0[16];
  nfcOk = ntagReadBlock(0, b0);
  if (nfcOk) {
    Serial.printf("[NFC] CC = %02X %02X %02X %02X\n", b0[12], b0[13], b0[14], b0[15]);
  } else {
    Serial.println("[NFC] NTAG not found");
  }

  attachInterrupt(digitalPinToInterrupt(SHUTTER),  isrShutter, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_MODE), isrMode,    FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_UP),   isrUp,      FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_DOWN), isrDown,    FALLING);

  char buf[24];
  formatDateTime(bootEpoch, buf, sizeof(buf));
  Serial.printf("[BOOT] %s, frames=%lu\n", buf, (unsigned long)frameCount);

  lastInputMs = millis();
  showToast("READY", 800);
}

void loop() {
  processShots();
  handleButtons();
  checkUiTimeout();
  updateDisplay();
}
