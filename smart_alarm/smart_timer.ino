/*
  ESP32 Smart Timer with DS3231 RTC and SH1106 Display
  
  Description:
  - WiFi web server for UI control
  - I2C communication with DS3231 RTC module
  - I2C communication with SH1106 OLED display
  - Set current time and scheduling delays
  - Real-time display with timer countdown
  
  Hardware:
  - ESP32 Dev Module
  - DS3231 RTC Module (I2C: SDA=21, SCL=22)
  - SH1106 Display (I2C: SDA=4, SCL=5)
  
  Libraries needed:
  - Wire (built-in)
  - WiFi (built-in)
  - WebServer (built-in)
  - U8g2 (universal graphics library - search "U8g2" in Arduino IDE)
  - RTClib (Adafruit)
*/

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>
#include "RTClib.h"
#include <EEPROM.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ==================== CONFIGURATION ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// I2C Pins for ESP32 - Bus 0 (RTC)
#define SDA_PIN 21
#define SCL_PIN 22

// I2C Pins for ESP32 - Bus 1 (OLED Display)
#define SDA_PIN_OLED 4
#define SCL_PIN_OLED 5

// WiFi Configuration
const char* ssid = "SMART_TIMER";
const char* password = "timer12345";

// Timer Pin (output for relay/buzzer)
#define TIMER_OUTPUT_PIN 26
#define BUTTON_PIN 25

// ==================== GLOBAL OBJECTS ====================
// Create U8g2 display object for SH1106 (128x64) using Software I2C
// F_SW_I2C is full-buffer mode which handles SH1106 offset issues better
// Parameters: rotation, clock_pin (GPIO5), data_pin (GPIO4), reset_pin
U8G2_SH1106_128X64_NONAME_F_SW_I2C display(U8G2_R0, SCL_PIN_OLED, SDA_PIN_OLED, U8X8_PIN_NONE);
RTC_DS3231 rtc;
WebServer server(80);
Preferences prefs;

// flag used when RTC initialization fails to avoid reset loops
bool rtcAvailable = true;
bool displayAvailable = true;  // track if display initialized successfully
bool displayPoweredOn = true;

// ==================== GLOBAL VARIABLES ====================
#define MAX_PERIODS 210
#define EXAM_MAX_PERIODS 100
#define HOLIDAY_MAX_DATES 15

// ==================== PREFERENCES STORAGE ====================
// Preferences (NVS) are used for reliability. EEPROM is only used for one-time migration.
#define PREFS_NAMESPACE "smart_timer"
#define PREFS_MAGIC_KEY "magic"
#define PREFS_MAGIC_VALUE 0xB1
#define PREFS_TIMER_KEY "timer"
#define PREFS_EXAM_KEY "exam"
#define PREFS_HOLIDAY_KEY "holiday"
#define PREFS_EXAMEN_KEY "examEn"

// ==================== EEPROM MIGRATION LAYOUT ====================
// Old EEPROM layout used EEPROM_SIZE=1024 with hard-coded offsets (400/700). With MAX_PERIODS=210,
// regular periods alone require 2 + 210*6 = 1262 bytes, which overwrote exam/holiday data and
// exceeded the 1024-byte EEPROM region. That corruption matches "junk periods" and unstable
// delete/list behavior.
#define PERIOD_EEPROM_BYTES 6
#define EEPROM_SIZE 4096

#define TIMER_DATA_OFFSET 0
#define TIMER_HEADER_BYTES 2
#define TIMER_PERIODS_OFFSET (TIMER_DATA_OFFSET + TIMER_HEADER_BYTES)
#define TIMER_DATA_BYTES (TIMER_HEADER_BYTES + (MAX_PERIODS * PERIOD_EEPROM_BYTES))

#define EXAM_DATA_OFFSET (TIMER_DATA_OFFSET + TIMER_DATA_BYTES)
#define EXAM_HEADER_BYTES 3
#define EXAM_PERIODS_OFFSET (EXAM_DATA_OFFSET + EXAM_HEADER_BYTES)
#define EXAM_DATA_BYTES (EXAM_HEADER_BYTES + (EXAM_MAX_PERIODS * PERIOD_EEPROM_BYTES))

#define HOLIDAY_DATA_OFFSET (EXAM_DATA_OFFSET + EXAM_DATA_BYTES)
#define HOLIDAY_ENTRY_BYTES 4

#define EEPROM_MAGIC_ADDR (EEPROM_SIZE - 1)
// Bumped to force a one-time migration path from the old, overlapping layout.
#define EEPROM_MAGIC_VALUE 0xA6

// Old layout support (best-effort migration)
#define EEPROM_OLD_MAGIC_VALUE 0xA5
#define EEPROM_OLD_SIZE 1024
#define EEPROM_OLD_MAGIC_ADDR (EEPROM_OLD_SIZE - 1)
#define EEPROM_OLD_EXAM_DATA_OFFSET 400
#define EEPROM_OLD_HOLIDAY_DATA_OFFSET 700

struct Period {
  uint8_t day; // 0=Sun, 1=Mon, 2=Tue, 3=Wed, 4=Thu, 5=Fri, 6=Sat
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint16_t delaySeconds;
};

struct TimerData {
  Period periods[MAX_PERIODS];
  uint8_t numPeriods = 0;
  bool timerActive = false;
};

struct ExamData {
  Period periods[MAX_PERIODS];
  uint8_t numPeriods = 0;
  bool timerActive = false;
};

struct HolidayDate {
  uint16_t year;
  uint8_t month;
  uint8_t day;
};

struct HolidayData {
  HolidayDate dates[HOLIDAY_MAX_DATES];
  uint8_t numDates = 0;
};

struct StoredTimerData {
  uint8_t numPeriods;
  uint8_t timerActive;
  Period periods[MAX_PERIODS];
} __attribute__((packed));

struct StoredExamData {
  uint8_t numPeriods;
  uint8_t timerActive;
  Period periods[MAX_PERIODS];
} __attribute__((packed));

struct StoredHolidayData {
  uint8_t numDates;
  HolidayDate dates[HOLIDAY_MAX_DATES];
} __attribute__((packed));

struct RelayState {
  bool isActive = false;
  unsigned long activationTime = 0;
  uint32_t delayDuration = 0;
};

TimerData timerData;
ExamData examData;
HolidayData holidayData;
bool examModeEnabled = false;
RelayState relayState;
unsigned long lastDisplayUpdate = 0;
unsigned long lastScheduleCheck = 0;

// button state tracking
bool apActive = true;
int lastButtonState = HIGH;
unsigned long buttonPressStart = 0;

// scrolling support for wifi/ssid text on display
int wifiScrollOffset = SCREEN_WIDTH;

// ==================== FORWARD DECLARATIONS ====================
void loadDataFromPreferences();
void saveDataToPreferences();
void setupWebServer();
void handleRoot();
void handleGetTime();
void handleSetTime();
void handleGetPeriods();
void handleGetHolidays();
void handleAddPeriod();
void handleAddHoliday();
void handleDeletePeriod();
void handleDeleteHoliday();
void handleClearPeriods();
void handleClearHolidays();
void handleEditPeriod();
void handleStartTimer();
void handleStopTimer();
void handleTrigger();
void handleNotFound();
void handleSetExamMode();
void handleGetExamPeriods();
void handleAddExamPeriod();
void handleDeleteExamPeriod();
void handleClearExamPeriods();
void handleEditExamPeriod();
void handleStartExamTimer();
void handleStopExamTimer();
void checkAndExecuteSchedules();
void updateDisplay();
void handleButtonPress();
const char* getHTMLContent();
bool isHolidayToday(const DateTime &now);
bool isValidStoredPeriod(const Period &p);
bool periodEquals(const Period &a, const Period &b);
bool hasDuplicateInTimerData(const Period &p, int uptoExclusive);
bool migrateFromEepromIfPresent();

// ==================== INITIALIZATION ====================
void setup() {
  Serial.begin(115200);
  delay(500);
  //Serial.print.println("\n\n=== ESP32 Smart Timer Initialization ===");
  
  // Disable watchdog timer to prevent reset issues during relay activation
  // The watchdog would trigger if the main loop is blocked for too long
  disableWatchdog();
  
  // Initialize Preferences for storing timer data
  prefs.begin(PREFS_NAMESPACE, false);
  //Serial.print.println("[SETUP] Preferences initialized");
  
  // Initialize GPIO early
  pinMode(TIMER_OUTPUT_PIN, OUTPUT);
  digitalWrite(TIMER_OUTPUT_PIN, LOW);

  // initialise button input
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize I2C Bus 0 (for RTC) with timeout
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);
  //Serial.print.println("[SETUP] I2C (RTC) started");
  
  // Initialize I2C Bus 1 (for Display) - separate bus
  display.begin();
  //Serial.print.println("[SETUP] Display I2C began");
  delay(100);
  
  // Try to initialize display - with timeout protection
  if (!initializeDisplay()) {
    displayAvailable = false;
  }
  
  // Initialize RTC
  //Serial.print.println("[SETUP] Initializing RTC...");
  initializeRTC();
  
  // Load saved data from EEPROM
  loadDataFromPreferences();
  
  // Initialize WiFi
  //Serial.print.println("[SETUP] Starting WiFi AP...");
  initializeWiFi();

  // display startup information on OLED
  showStartupInfo();
  
  // Setup Web Server routes
  //Serial.print.println("[SETUP] Setting up web server routes...");
  setupWebServer();
  
  //Serial.print.println("[SETUP] ============ INITIALIZATION COMPLETE ============");
  delay(1000);
}

// ==================== MAIN LOOP ====================
void loop() {
  server.handleClient();
  
  // watch user button
  checkButton();

  // Check and manage relay state (non-blocking)
  checkRelayState();
  
  // Update display every 500ms
  if (millis() - lastDisplayUpdate >= 500) {
    ////Serial.print.println("[LOOP] calling updateDisplay");
    updateDisplay();
    lastDisplayUpdate = millis();
  }
  
  // Check timer every 500ms (only if RTC is available)
  if (millis() - lastScheduleCheck >= 500) {
    if (rtcAvailable) {
      checkAndExecuteTimer();
    }
    lastScheduleCheck = millis();
  }
}

/* Function to start wifi*/
void startWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  apActive = true;
}

/* Function to stop wifi*/
void stopWiFiAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apActive = false;
}

// called from loop() to watch the user button
void checkButton() {
  int state = digitalRead(BUTTON_PIN);
  if (state != lastButtonState) {
    if (state == LOW) {                // button pressed
      buttonPressStart = millis();
    } else {                           // button released
      unsigned long dur = millis() - buttonPressStart;
      if (dur >= 5000) {               // long press – toggle OLED
        if (displayAvailable) {
          if (displayPoweredOn && apActive) {
            display.setPowerSave(1);    // turn display off
            displayPoweredOn = false;
            stopWiFiAP();
          } 
          else {
            display.setPowerSave(0);    // turn display back on
            displayPoweredOn = true;
            startWiFiAP();
          }
        }
      } else if (dur >= 1000) {        // medium press – 2‑second output
        triggerTimer(10);
      }
    }
    lastButtonState = state;
  }
}

// ==================== DISPLAY FUNCTIONS ====================
// Initialize display with timeout protection - returns true if successful
bool initializeDisplay() {
  if (!displayAvailable) return false;
  
  try {
    display.enableUTF8Print();
    display.clearBuffer();
    display.setFont(u8g2_font_7x13_tf);
    display.drawStr(0, 10, "Powered by");
    display.setFont(u8g2_font_10x20_tf);
    display.drawStr(16, 30, "Ayinostech");
    display.sendBuffer();
    delay(2500);
    //Serial.print.println("[DISP] Display initialized successfully");
    return true;
  } catch (...) {
    //Serial.print.println("[DISP] ERROR: Display initialization failed");
    displayAvailable = false;
    return false;
  }
}

void safeDisplayDrawStr(uint8_t x, uint8_t y, const char* str) {
  if (!displayAvailable) return;
  display.drawStr(x, y, str);
}

void safeDisplayClearBuffer() {
  if (!displayAvailable) return;
  display.clearBuffer();
}

void safeDisplaySetFont(const uint8_t *font) {
  if (!displayAvailable) return;
  display.setFont(font);
}

void safeDisplaySendBuffer() {
  if (!displayAvailable) return;
  display.sendBuffer();
}

void safeDisplayDrawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
  if (!displayAvailable) return;
  display.drawLine(x1, y1, x2, y2);
}

void updateDisplay() {
  if (!displayAvailable || !displayPoweredOn) return;
  
  DateTime now = rtcAvailable ? rtc.now() : DateTime(2000,1,1,0,0,0);
  char buffer[64];
  
  safeDisplayClearBuffer();
  
  // Line 1: date with nice formatting (Y=10)
  safeDisplaySetFont(u8g2_font_7x13_tf);
  const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  snprintf(buffer, sizeof(buffer), "%s %d/%d/%04d  %s", dayNames[now.dayOfTheWeek()], now.day(), now.month(), now.year(), examModeEnabled ? "Exm" : "Reg");
  safeDisplayDrawStr(0, 10, buffer);
  if (rtcAvailable && rtc.lostPower()) {
    safeDisplayDrawStr(100, 10, "LOW");
  }
  safeDisplayDrawLine(0, 12, SCREEN_WIDTH, 12);
  
  // Line 2: large time display (Y=32) - centered and prominent
  safeDisplaySetFont(u8g2_font_10x20_tf);
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  if (displayAvailable) {
    uint8_t timeWidth = display.getStrWidth(buffer);
    uint8_t timeX = (SCREEN_WIDTH - timeWidth) / 2;  // center horizontally
    safeDisplayDrawStr(timeX, 32, buffer);
  }
  
  // Line 3: status and next scheduled (Y=48)
  safeDisplaySetFont(u8g2_font_6x10_tf);
  // Show exam-mode status when enabled
  int periodsCount = examModeEnabled ? examData.numPeriods : timerData.numPeriods;
  bool periodsActive = examModeEnabled ? examData.timerActive : timerData.timerActive;
  snprintf(buffer, sizeof(buffer), "%sPeriods:%d Active:%s", examModeEnabled ? "" : "", periodsCount, periodsActive ? "YES" : "NO");
  safeDisplayDrawStr(0, 48, buffer);

  // compute next scheduled event for the active mode
  uint8_t currentDay = now.dayOfTheWeek();
  uint32_t currentSeconds = (now.hour() * 3600) + (now.minute() * 60) + now.second();
  int nextIndex = -1;
  uint32_t minDiff = UINT32_MAX;
  if (examModeEnabled) {
    for (int i = 0; i < examData.numPeriods; i++) {
      if (examData.periods[i].day == currentDay) {
        uint32_t periodSeconds = (examData.periods[i].hour * 3600) + (examData.periods[i].minute * 60) + examData.periods[i].second;
        if (periodSeconds >= currentSeconds) {
          uint32_t diff = periodSeconds - currentSeconds;
          if (diff < minDiff) {
            minDiff = diff;
            nextIndex = i;
          }
        }
      }
    }
    if (nextIndex >= 0 && !isHolidayToday(rtc.now())) {
      snprintf(buffer, sizeof(buffer), periodsActive ? "Next: %02d:%02d:%02d" : "", 
               examData.periods[nextIndex].hour,
               examData.periods[nextIndex].minute,
               examData.periods[nextIndex].second);
      safeDisplayDrawStr(0, 60, buffer);
    }
    else if (isHolidayToday(rtc.now())) {
      snprintf(buffer, sizeof(buffer), "Holiday Today"); 
      safeDisplayDrawStr(20, 60, buffer);
    }
    else {
      snprintf(buffer, sizeof(buffer), ""); 
      safeDisplayDrawStr(20, 60, buffer);
    }

  } else {
    for (int i = 0; i < timerData.numPeriods; i++) {
      if (timerData.periods[i].day == currentDay) {
        uint32_t periodSeconds = (timerData.periods[i].hour * 3600) + (timerData.periods[i].minute * 60) + timerData.periods[i].second;
        if (periodSeconds >= currentSeconds) {
          uint32_t diff = periodSeconds - currentSeconds;
          if (diff < minDiff) {
            minDiff = diff;
            nextIndex = i;
          }
        }
      }
    }
    if (nextIndex >= 0 && !isHolidayToday(rtc.now())) {
      snprintf(buffer, sizeof(buffer), periodsActive ? "Next: %02d:%02d:%02d" : "", 
               timerData.periods[nextIndex].hour,
               timerData.periods[nextIndex].minute,
               timerData.periods[nextIndex].second);
      safeDisplayDrawStr(0, 60, buffer);
    }
    else if (isHolidayToday(rtc.now())) {
      snprintf(buffer, sizeof(buffer), "Holiday Today"); 
      safeDisplayDrawStr(20, 60, buffer);
    }
    else {
      snprintf(buffer, sizeof(buffer), ""); 
      safeDisplayDrawStr(20, 60, buffer);
    }
  }

  // scrolling wifi/ssid info at very bottom
//   {
//     IPAddress ip = WiFi.softAPIP();
//     char wifiLine[64];
//     snprintf(wifiLine, sizeof(wifiLine), "SSID:%s IP:%d.%d.%d.%d", ssid, ip[0], ip[1], ip[2], ip[3]);
//     uint8_t lineWidth = display.getStrWidth(wifiLine);
//     int16_t x = wifiScrollOffset - lineWidth;
//     safeDisplayDrawStr(x, SCREEN_HEIGHT - 1, wifiLine);
//     wifiScrollOffset--;
//     if (wifiScrollOffset < 0) wifiScrollOffset = SCREEN_WIDTH;
//   }

  safeDisplaySendBuffer();
}



// ==================== RTC FUNCTIONS ====================
// helpers for date validation on the ESP32 side
bool isLeapYear(int yr) {
  return ((yr % 4 == 0) && (yr % 100 != 0)) || (yr % 400 == 0);
}

int daysInMonth(int yr, int mo) {
  if (mo < 1 || mo > 12) return 0;
  if (mo == 2) return isLeapYear(yr) ? 29 : 28;
  if (mo == 4 || mo == 6 || mo == 9 || mo == 11) return 30;
  return 31;
}

// helper routines to centralize validation rules used by several APIs
bool isValidTime(int h, int m, int s) {
  return (h >= 0 && h <= 23) && (m >= 0 && m <= 59) && (s >= 0 && s <= 59);
}

bool isValidDelay(int d) {
  // delay is measured in seconds; new requirement allows 0..60
  return (d >= 0 && d <= 60);
}

// convert day value received from client into internal 0-6 range
// accepts either 0-6 or 1-7; returns -1 when invalid
int normalizeDay(int d) {
  if (d >= 1 && d <= 7) return d - 1;
  if (d >= 0 && d <= 6) return d;
  return -1;
}

static inline bool isValidStoredDay(uint8_t d) { return d <= 6; }

bool isValidStoredPeriod(const Period &p) {
  return isValidStoredDay(p.day) &&
         isValidTime((int)p.hour, (int)p.minute, (int)p.second) &&
         isValidDelay((int)p.delaySeconds);
}

bool periodEquals(const Period &a, const Period &b) {
  return a.day == b.day &&
         a.hour == b.hour &&
         a.minute == b.minute &&
         a.second == b.second &&
         a.delaySeconds == b.delaySeconds;
}

bool hasDuplicateInTimerData(const Period &p, int uptoExclusive) {
  for (int i = 0; i < uptoExclusive; i++) {
    if (periodEquals(timerData.periods[i], p)) return true;
  }
  return false;
}

void initializeRTC() {
  if (!rtc.begin()) {
    rtcAvailable = false;
    //Serial.print.println("[RTC] ERROR: RTC not found!");
    if (displayAvailable) {
      safeDisplayClearBuffer();
      safeDisplaySetFont(u8g2_font_6x10_tf);
      safeDisplayDrawStr(10, 20, "ERROR:");
      safeDisplayDrawStr(10, 35, "RTC not found!");
      safeDisplaySendBuffer();
    }
    // do not hang; allow device to continue so watchdog won't trigger
    return;
  }
  if (rtc.lostPower()) {
    //Serial.print.println("[RTC] RTC lost power, setting default time");
    // Set to compile time if RTC lost power
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  //Serial.print.println("[RTC] RTC initialized successfully");
}

DateTime getRTCTime() {
  if (rtcAvailable) return rtc.now();
  // return dummy time when no RTC
  return DateTime(2000,1,1,0,0,0);
}

void setRTCTime(uint8_t hour, uint8_t minute, uint8_t second) {
  DateTime now = rtc.now();
  rtc.adjust(DateTime(now.year(), now.month(), now.day(), hour, minute, second));
  ////Serial.print.printf("RTC time set to: %02d:%02d:%02d\n", hour, minute, second);
}

// ==================== TIMER FUNCTIONS ====================
bool isHolidayToday(const DateTime &now) {
  for (int i = 0; i < holidayData.numDates; i++) {
    const HolidayDate &h = holidayData.dates[i];
    if (h.year == now.year() && h.month == now.month() && h.day == now.day()) {
      return true;
    }
  }
  return false;
}

void checkAndExecuteTimer() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    if (isHolidayToday(now)) {
      // Suppress all timer actions on holiday dates and ensure relay stays OFF.
      if (relayState.isActive) {
        digitalWrite(TIMER_OUTPUT_PIN, LOW);
        relayState.isActive = false;
      }
      return;
    }
  }

  // Check normal timer
  if (timerData.timerActive && rtcAvailable) {
    DateTime now = rtc.now();
    uint8_t currentDay = now.dayOfTheWeek(); // 0=Sun, 1=Mon, ..., 6=Sat
    uint32_t currentSeconds = (now.hour() * 3600) + (now.minute() * 60) + now.second();
    
    static uint32_t lastTriggeredSeconds = UINT32_MAX;
    
    // Check if we've already triggered this second
    if (lastTriggeredSeconds != currentSeconds) {
      for (int i = 0; i < timerData.numPeriods; i++) {
        Period &p = timerData.periods[i];
        if (p.day == currentDay) {
          uint32_t periodSeconds = (p.hour * 3600) + (p.minute * 60) + p.second;
          if (currentSeconds == periodSeconds) {
            ////Serial.print.printf("[TIMER] Triggered at %02d:%02d:%02d on day %d (delay=%d sec)\n", 
                          // p.hour, p.minute, p.second, p.day, p.delaySeconds);
            triggerTimer(p.delaySeconds);
            lastTriggeredSeconds = currentSeconds;
            break; // Trigger only one at a time
          }
        }
      }
    }
  }
  
  // Check exam timer
  if (examData.timerActive && rtcAvailable) {
    DateTime now = rtc.now();
    uint8_t currentDay = now.dayOfTheWeek(); // 0=Sun, 1=Mon, ..., 6=Sat
    uint32_t currentSeconds = (now.hour() * 3600) + (now.minute() * 60) + now.second();
    
    static uint32_t lastExamTriggeredSeconds = UINT32_MAX;
    
    // Check if we've already triggered this second
    if (lastExamTriggeredSeconds != currentSeconds) {
      for (int i = 0; i < examData.numPeriods; i++) {
        Period &p = examData.periods[i];
        if (p.day == currentDay) {
          uint32_t periodSeconds = (p.hour * 3600) + (p.minute * 60) + p.second;
          if (currentSeconds == periodSeconds) {
            ////Serial.print.printf("[EXAM TIMER] Triggered at %02d:%02d:%02d on day %d (delay=%d sec)\n", 
                          // p.hour, p.minute, p.second, p.day, p.delaySeconds);
            triggerTimer(p.delaySeconds);
            lastExamTriggeredSeconds = currentSeconds;
            break; // Trigger only one at a time
          }
        }
      }
    }
  }
}

void triggerTimer(uint16_t delaySeconds) {
  //Serial.print.printf("[RELAY] Activating output for %d seconds\n", delaySeconds);
  relayState.isActive = true;
  relayState.activationTime = millis();
  relayState.delayDuration = delaySeconds * 1000;
  digitalWrite(TIMER_OUTPUT_PIN, HIGH);
  
  // Flash display - visual feedback with U8g2
  if (displayAvailable) {
    safeDisplayClearBuffer();
    safeDisplaySetFont(u8g2_font_ncenB14_tr);
    safeDisplayDrawStr(20, 30, "LOAD ON!");
    safeDisplaySendBuffer();
    delay(delaySeconds * 1000); // Short flash only, not blocking
    
    safeDisplayClearBuffer();
    safeDisplaySendBuffer();
  }
}

void checkRelayState() {
  if (relayState.isActive) {
    unsigned long elapsed = millis() - relayState.activationTime;
    if (elapsed >= relayState.delayDuration) {
      digitalWrite(TIMER_OUTPUT_PIN, LOW);
      relayState.isActive = false;
      //Serial.print.println("[RELAY] Output deactivated");
    }
  }
}

void disableWatchdog() {
  // Disable watchdog timer to prevent resets during long relay activations
  // The watchdog on ESP32 by default resets if loop() blocks for ~8 seconds
  // We disable it because we handle relay timing with millis() instead of blocking delay()
  esp_task_wdt_deinit();
}

// ==================== WiFi FUNCTIONS ====================
void initializeWiFi() {
  ////Serial.print.printf("Starting WiFi as AP: %s\n", ssid);
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  ////Serial.print.print("AP IP address: ");
  ////Serial.print.println(IP);
  
  delay(1000);
}

// display initial information on the OLED after setup is mostly complete
void showStartupInfo() {
  if (!displayAvailable) return;

  IPAddress ip = WiFi.softAPIP();
  char buffer[64];

  safeDisplayClearBuffer();
  safeDisplaySetFont(u8g2_font_6x10_tf);
  safeDisplayDrawStr(0, 10, "Smart Timer Ready");

  // SSID
  snprintf(buffer, sizeof(buffer), "SSID: %s", ssid);
  safeDisplayDrawStr(0, 22, buffer);

  // IP address
  snprintf(buffer, sizeof(buffer), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  safeDisplayDrawStr(0, 34, buffer);

  // timer configuration summary
  snprintf(buffer, sizeof(buffer), "Periods: %d", timerData.numPeriods);
  safeDisplayDrawStr(0, 46, buffer);

  snprintf(buffer, sizeof(buffer), "Timer %s", timerData.timerActive ? "Active" : "Inactive");
  safeDisplayDrawStr(0, 58, buffer);

  safeDisplaySendBuffer();
  delay(3000); // hold the info for a few seconds
}


// ==================== WEB SERVER FUNCTIONS ====================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/time", handleGetTime);
  server.on("/api/settime", handleSetTime);
  server.on("/api/periods", handleGetPeriods);
  server.on("/api/holidays", handleGetHolidays);
  server.on("/api/addperiod", handleAddPeriod);
  server.on("/api/addholiday", handleAddHoliday);
  server.on("/api/deleteperiod", handleDeletePeriod);
  server.on("/api/deleteholiday", handleDeleteHoliday);
  server.on("/api/clearperiods", handleClearPeriods);
  server.on("/api/clearholidays", handleClearHolidays);
  server.on("/api/editperiod", handleEditPeriod);      // new endpoint for editing
  server.on("/api/starttimer", handleStartTimer);
  server.on("/api/stoptimer", handleStopTimer);
  server.on("/api/trigger", handleTrigger);            // manual trigger API
  
  // Exam mode endpoints
  server.on("/api/setexammode", handleSetExamMode);
  server.on("/api/examperiods", handleGetExamPeriods);
  server.on("/api/addexamperiod", handleAddExamPeriod);
  server.on("/api/deleteexamperiod", handleDeleteExamPeriod);
  server.on("/api/clearexamperiods", handleClearExamPeriods);
  server.on("/api/editexamperiod", handleEditExamPeriod);
  server.on("/api/startexamtimer", handleStartExamTimer);
  server.on("/api/stopexamtimer", handleStopExamTimer);
  
  server.onNotFound(handleNotFound);
  
  server.begin();
  ////Serial.print.println("Web server started on port 80");
}

void handleRoot() {
  server.send_P(200, "text/html", getHTMLContent());
}

const char* getHTMLContent() {
  static const char html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Timer Controller</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        
        .container {
            background: white;
            border-radius: 20px;
            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
            max-width: 500px;
            width: 100%;
            padding: 40px;
        }
        
        h1 {
            color: #333;
            margin-bottom: 10px;
            text-align: center;
            font-size: 28px;
        }
        
        .subtitle {
            text-align: center;
            color: #00008B;
            margin-bottom: 30px;
            font-size: 14px;
        }
        
        .section {
            margin-bottom: 30px;
            padding: 20px;
            background: #f8f9fa;
            border-radius: 10px;
            border-left: 4px solid #667eea;
            border-right: 4px solid #667eea;
        }
        
        .section-title {
            font-size: 16px;
            font-weight: 600;
            color: #333;
            margin-bottom: 15px;
            display: flex;
            align-items: center;
        }
        
        .section-title::before {
            content: '';
            width: 6px;
            height: 6px;
            background: #667eea;
            border-radius: 50%;
            margin-right: 10px;
        }
        
        .time-input-group {
            display: flex;
            gap: 10px;
            justify-content: center;
            margin-bottom: 15px;
        }
        
        .time-input {
            width: 80px;
            padding: 12px;
            border: 2px solid #ddd;
            border-radius: 8px;
            font-size: 18px;
            text-align: center;
            font-weight: 600;
            color: #333;
            transition: all 0.3s ease;
        }
        .date-time-input-group {
            display: flex;
            gap: 5px;
            justify-content: center;
            align-items: center;
            flex-wrap: wrap;
        }
        
        .time-input:focus {
            outline: none;
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
        }
        
        .time-input::placeholder {
            color: #999;
        }
        
        .separator {
            color: #666;
            font-size: 20px;
            font-weight: 600;
            display: flex;
            align-items: center;
        }
        
        .label {
            font-size: 12px;
            color: #666;
            margin-top: 5px;
            text-align: center;
        }
        
        .button-group {
            display: flex;
            gap: 10px;
            margin-top: 20px;
        }
        
        button {
            flex: 1;
            padding: 12px;
            border: none;
            border-radius: 8px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s ease;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        
        .btn-primary {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            box-shadow: 0 4px 15px rgba(102, 126, 234, 0.4);
        }
        
        .btn-primary:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(102, 126, 234, 0.6);
        }
        
        .btn-primary:active {
            transform: translateY(0);
        }
        
        .btn-secondary {
            background: #e0e0e0;
            color: #333;
        }
        
        .btn-secondary:hover {
            background: #d0d0d0;
        }
        
        .btn-danger {
            background: #ff6b6b;
            color: white;
        }
        
        .btn-danger:hover {
            background: #ff5252;
        }
        
        .status-display {
            background: white;
            padding: 15px;
            border-radius: 8px;
            text-align: center;
            margin-top: 15px;
            border: 2px solid #ddd;
        }
        
        .status-label {
            font-size: 12px;
            color: #666;
            margin-bottom: 5px;
        }
        
        .status-value {
            font-size: 18px;
            font-weight: 600;
            color: #333;
            font-family: 'Courier New', monospace;
        }
        
        .timer-status {
            padding: 10px;
            border-radius: 5px;
            text-align: center;
            font-weight: 600;
            margin-top: 15px;
        }
        
        .status-inactive {
            background: #ffebee;
            color: #c62828;
        }
        
        .status-active {
            background: #e8f5e9;
            color: #2e7d32;
            animation: pulse 1.5s infinite;
        }
        
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.7; }
        }
        
        .loading {
            display: inline-block;
            width: 8px;
            height: 8px;
            background: currentColor;
            border-radius: 50%;
            margin-right: 5px;
            animation: loading 1.5s infinite;
        }
        
        @keyframes loading {
            0%, 100% { opacity: 0.3; }
            50% { opacity: 1; }
        }
        
        .info-box {
            background: #e3f2fd;
            border-left: 4px solid #2196f3;
            padding: 12px;
            border-radius: 5px;
            margin-top: 15px;
            font-size: 12px;
            color: #1565c0;
            line-height: 1.6;
        }
        
        .error {
            background: #ffebee;
            border-left: 4px solid #f44336;
            padding: 12px;
            border-radius: 5px;
            margin-top: 15px;
            font-size: 12px;
            color: #c62828;
            display: none;
        }
        
        .success {
            background: #e8f5e9;
            border-left: 4px solid #4caf50;
            padding: 12px;
            border-radius: 5px;
            margin-top: 15px;
            font-size: 12px;
            color: #2e7d32;
            display: none;
        }
        
        /* Toggle Switch Styles */
        .toggle-mode-container {
            display: flex;
            align-items: center;
            justify-content: center;
            margin-bottom: 20px;
            gap: 15px;
        }
        
        .mode-label {
            font-size: 14px;
            font-weight: 600;
            color: #333;
            min-width: 80px;
            text-align: right;
        }
        
        .toggle-switch {
            position: relative;
            display: inline-block;
            width: 50px;
            height: 28px;
        }
        
        .toggle-switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        
        .toggle-slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #ccc;
            transition: 0.4s;
            border-radius: 34px;
        }
        
        .toggle-slider:before {
            position: absolute;
            content: "";
            height: 22px;
            width: 22px;
            left: 3px;
            bottom: 3px;
            background-color: white;
            transition: 0.4s;
            border-radius: 50%;
        }
        
        input:checked + .toggle-slider {
            background-color: #667eea;
        }
        
        input:checked + .toggle-slider:before {
            transform: translateX(22px);
        }
        
        .exam-mode-label {
            font-size: 14px;
            font-weight: 600;
            color: #00;
            min-width: 80px;
        }
        
        /* Hidden sections */
        .normal-mode-section {
            display: block;
        }
        
        .normal-mode-section.hidden {
            display: none;
        }
        
        .exam-mode-section {
            display: none;
        }
        
        .exam-mode-section.visible {
            display: block;
        }

        .delete-btn {
            background: #ff6b6b;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 5px 12px;
            font-size: 12px;
            cursor: pointer;
            flex: 0 0 auto;
        }

        .title-gold {
            font-size: 26px;
            background: linear-gradient(45deg,#FFD700,#D4AF37,#B8962E);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        
        @media (max-width: 480px) {
            .container {
                padding: 25px;
            }
            
            h1 {
                font-size: 22px;
            }
            
            .time-input {
                width: 70px;
                font-size: 16px;
            }
            
            .toggle-mode-container {
                flex-direction: row;
                gap: 10px;
            }
            
            .mode-label {
                text-align: center;
                min-width: auto;
            }
            
            .exam-mode-label {
                text-align: center;
                min-width: auto;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1 class="title-gold">⏱ Smart School Timer</h1>
        <p class="subtitle">Powered by Ayinostech</p>
        
        <!-- Mode Toggle Button -->
        <div class="toggle-mode-container">
            <span class="mode-label">Regular</span>
            <label class="toggle-switch">
                <input type="checkbox" id="examModeToggle" onchange="toggleExamMode()">
                <span class="toggle-slider"></span>
            </label>
            <span class="exam-mode-label">Exam</span>
        </div>
        
        <!-- Current Time Display -->
        <div class="section normal-mode-section" id="timeDisplaySection">
            <div class="section-title">Current Date & Time on Timer</div>
            <div class="status-display">
                <div class="status-label">Current Date & Time</div>
                <div class="status-value" id="currentTime" style="font-size: 16px;">--/--/-- --:--:--</div>
                <div class="status-label" style="margin-top: 8px;">Day of Week</div>
                <div class="status-value" id="currentDayOfWeek">---</div>
            </div>
        </div>
        
        <!-- Set Time Section -->
        <div class="section normal-mode-section" id="setTimeSection">
            <div class="section-title">Set Current Date & Time<br>(YYYY-MM-DD HH:MM:SS)</div>
            <div class="date-time-input-group" style="margin-bottom: 15px;">
                <input type="number" id="setYear" class="time-input" min="2020" max="2099" placeholder="2026" style="width: 100px;">
                <span class="separator">-</span>
                <input type="number" id="setMonth" class="time-input" min="1" max="12" placeholder="01" style="width: 70px;">
                <span class="separator">-</span>
                <input type="number" id="setDay" class="time-input" min="1" max="31" placeholder="01" style="width: 70px;">
            </div>
            <div class="date-time-input-group" style="margin-bottom: 15px;">
                <input type="number" id="setHour" class="time-input" min="0" max="23" placeholder="00">
                <span class="separator">:</span>
                <input type="number" id="setMinute" class="time-input" min="0" max="59" placeholder="00">
                <span class="separator">:</span>
                <input type="number" id="setSecond" class="time-input" min="0" max="59" placeholder="00">
            </div>
            <div style="margin-bottom: 15px; text-align: center;">
                <label style="font-size: 12px; color: #666; margin-right: 10px;">Day of Week:</label>
                <select id="setDayOfWeek" class="time-input" style="width: 120px;">
                    <option value="0">Sunday</option>
                    <option value="1">Monday</option>
                    <option value="2">Tuesday</option>
                    <option value="3">Wednesday</option>
                    <option value="4">Thursday</option>
                    <option value="5">Friday</option>
                    <option value="6">Saturday</option>
                </select>
            </div>
            <button class="btn-primary" onclick="setDateTime()">Set Date & Time</button>
        </div>
        
        <!-- Add Time Period Section -->
        <div class="section normal-mode-section" id="addPeriodSection">
            <div class="section-title">Add Scheduled Period</div>
            <!-- mode selector: which days should receive this period -->
            <div style="margin-bottom: 15px; text-align: center;">
                <label for="periodMode" style="font-size:12px; color:#666; margin-right:5px;">Apply to:</label>
                <select id="periodMode" class="time-input" style="width:150px; margin-right:10px;">
                    <option value="single" selected>Single day</option>
                    <option value="weekdays">Mon‑Fri</option>
                    <option value="mon_to_thu">Mon‑Thu</option>
                    <option value="everyday">Sun-Sat</option>
                </select>
            </div>
            <div style="margin-bottom: 15px;">
                <select id="periodDay" class="time-input" style="width: 130px; margin-right: 10px;">
                    <!-- values in UI are 1‑7 for clarity; converted to 0‑6 in JS -->
                    <option value="1">Sunday</option>
                    <option value="2">Monday</option>
                    <option value="3">Tuesday</option>
                    <option value="4">Wednesday</option>
                    <option value="5">Thursday</option>
                    <option value="6">Friday</option>
                    <option value="7">Saturday</option>
                </select>
                <span style="font-size: 12px; color: #666;">at</span>
            </div>
            <div class="time-input-group" style="display: flex; justify-content: center; gap: 5px; margin-bottom: 15px;">
                <div>
                    <input type="number" id="periodHour" class="time-input" min="0" max="23" placeholder="00">
                    <div class="label">Hour</div>
                </div>
                <div class="separator">:</div>
                <div>
                    <input type="number" id="periodMinute" class="time-input" min="0" max="59" placeholder="00">
                    <div class="label">Minute</div>
                </div>
                <div class="separator">:</div>
                <div>
                    <input type="number" id="periodSecond" class="time-input" min="0" max="59" placeholder="00">
                    <div class="label">Second</div>
                </div>
            </div>
            <div style="margin-bottom: 15px; text-align: center;">
                <span style="font-size: 12px; color: #666; margin-right: 10px;">Delay (0–60s):</span>
                <input type="number" id="periodDelay" class="time-input" min="0" max="60" placeholder="00" style="width: 100px;">
                <span style="font-size: 12px; color: #666; margin-left: 5px;">seconds</span>
            </div>
            <button id="addPeriodBtn" class="btn-primary" onclick="addPeriod()">Add Period</button>
        </div>
        
        <!-- Periods List Section -->
        <div class="section normal-mode-section" id="periodsListSection">
            <div class="section-title">Scheduled Periods</div>
            <div id="periodsList" class="status-display" style="text-align: left; max-height: 200px; overflow-y: auto;">
                <div class="status-label">No periods added yet.</div>
            </div>
            <div style="margin-top:8px; text-align: right;">
              <button class="btn-danger" style="padding:8px 12px; font-size:12px;" onclick="clearAllPeriods()">Clear All Periods</button>
            </div>
        </div>

        <!-- Holiday Dates Section (date only) -->
        <div class="section normal-mode-section" id="holidaySection">
            <div class="section-title">Holiday Dates (YYYY-MM-DD)</div>
            <div class="date-time-input-group" style="margin-bottom: 15px;">
                <input type="number" id="holidayYear" class="time-input" min="2000" max="2099" placeholder="2026" style="width: 100px;">
                <span class="separator">-</span>
                <input type="number" id="holidayMonth" class="time-input" min="1" max="12" placeholder="01" style="width: 70px;">
                <span class="separator">-</span>
                <input type="number" id="holidayDay" class="time-input" min="1" max="31" placeholder="01" style="width: 70px;">
            </div>
            <button class="btn-primary" onclick="addHoliday()">Add Holiday Date</button>
            <div id="holidayTodayBadge" class="status-label" style="margin-top: 10px; text-align:center; font-weight:600;"></div>
            <div id="holidaysList" class="status-display" style="text-align: left; max-height: 180px; overflow-y: auto; margin-top: 10px;">
                <div class="status-label">No holidays added yet.</div>
            </div>
            <div style="margin-top:8px; text-align: right;">
              <button class="btn-danger" style="padding:8px 12px; font-size:12px;" onclick="clearAllHolidays()">Clear All Holidays</button>
            </div>
        </div>
        
        <!-- Manual Trigger Section for real-time testing 
        <div class="section normal-mode-section" id="manualTriggerSection">
            <div class="section-title">Manual Trigger (Emergency Bell)</div>
            <div style="margin-bottom: 15px; text-align: center;">
                <span style="font-size: 12px; color: #666; margin-right: 10px;">Delay:</span>
                <input type="number" id="manualDelay" class="time-input" min="0" max="60" placeholder="60" style="width: 100px;">
                <span style="font-size: 12px; color: #666; margin-left: 5px;">seconds</span>
            </div>
            <button class="btn-secondary" onclick="triggerNow()">Trigger Now</button>
        </div>
        -->
        
        <!-- Timer Status Section -->
        <div class="section normal-mode-section" id="timerControlSection">
            <div class="section-title">Timer Control (ON/OFF)</div>
            <div id="timerStatus" class="timer-status status-inactive">
                ⭕ Timer is INACTIVE
            </div>
            <div class="button-group">
                <button class="btn-primary" onclick="startTimer()">Timer ON</button>
                <button class="btn-danger" onclick="stopTimer()">Timer OFF</button>
            </div>
        </div>
        
        <!-- EXAM MODE SECTIONS -->
        
        <!-- Add Exam Period Section -->
        <div class="section exam-mode-section" id="addExamPeriodSection">
            <div class="section-title">Add Exam Schedule</div>
            <!-- mode selector: only Mon-Fri, Mon-Thu, and Mon-Sat -->
            <div style="margin-bottom: 15px; text-align: center;">
                <label for="examPeriodMode" style="font-size:12px; color:#666; margin-right:5px;">Apply to:</label>
                <select id="examPeriodMode" class="time-input" style="width:150px; font-size:16px; margin-right:10px;">
                    <option value="weekdays" style="font-size:16px;" selected>Mon - Fri</option>
                    <option value="monfri" style="font-size:16px;">Mon - Sat</option>
                </select>
            </div>
            <div class="time-input-group" style="display: flex; justify-content: center; gap: 5px; margin-bottom: 15px;">
                <div>
                    <input type="number" id="examPeriodHour" class="time-input" min="0" max="23" placeholder="00">
                    <div class="label">Hour</div>
                </div>
                <div class="separator">:</div>
                <div>
                    <input type="number" id="examPeriodMinute" class="time-input" min="0" max="59" placeholder="00">
                    <div class="label">Minute</div>
                </div>
                <div class="separator">:</div>
                <div>
                    <input type="number" id="examPeriodSecond" class="time-input" min="0" max="59" placeholder="00">
                    <div class="label">Second</div>
                </div>
            </div>
            <div style="margin-bottom: 15px; text-align: center;">
                <span style="font-size: 12px; color: #666; margin-right: 10px;">Delay (0–60s):</span>
                <input type="number" id="examPeriodDelay" class="time-input" min="0" max="60" placeholder="00" style="width: 100px;">
                <span style="font-size: 12px; color: #666; margin-left: 5px;">seconds</span>
            </div>
            <button class="btn-primary" onclick="addExamPeriod()">Add Schedule</button>
        </div>
        
        <!-- Exam Periods List Section -->
        <div class="section exam-mode-section" id="examPeriodsListSection">
            <div class="section-title">Exam Schedules</div>
            <div id="examPeriodsList" class="status-display" style="text-align: left; max-height: 200px; overflow-y: auto;">
                <div class="status-label">No exam schedules added yet.</div>
            </div>
            <div style="margin-top:8px; text-align: right;">
              <button class="btn-danger" style="padding:8px 12px; font-size:12px;" onclick="clearAllExamPeriods()">Clear All</button>
            </div>
        </div>
        
        <!-- Exam Timer Control Section -->
        <div class="section exam-mode-section" id="examTimerControlSection">
            <div class="section-title">Exam Mode Timer</div>
            <div id="examTimerStatus" class="timer-status status-inactive">
                ⭕ Exam Timer is INACTIVE
            </div>
            <div class="button-group">
                <button class="btn-primary" onclick="startExamTimer()">Timer ON</button>
                <button class="btn-danger" onclick="stopExamTimer()">Timer OFF</button>
            </div>
        </div>
        
        <!-- Status Messages -->
        <div class="error" id="errorMessage"></div>
        <div class="success" id="successMessage"></div>
    </div>
    
    <script>
        const dayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
        const fullDayNames = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
        
        let currentMode = 'normal'; // 'normal' or 'exam'
        
        // Toggle between normal and exam mode
        function toggleExamMode() {
            const toggle = document.getElementById('examModeToggle');
            const newMode = toggle.checked ? 'exam' : 'normal';
            
            fetch('/api/setexammode?mode=' + (newMode === 'exam' ? 'true' : 'false'))
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        currentMode = newMode;
                        updateUIForMode(newMode);
                        showSuccess('✓ Switched to ' + (newMode === 'exam' ? 'EXAM' : 'NORMAL') + ' mode');
                    } else {
                        showError('✗ Failed to switch mode: ' + (data.message || 'Unknown error'));
                        toggle.checked = !toggle.checked;
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                    toggle.checked = !toggle.checked;
                });
        }
        
        // Update UI visibility based on mode
        function updateUIForMode(mode) {
            const normalSections = document.querySelectorAll('.normal-mode-section');
            const examSections = document.querySelectorAll('.exam-mode-section');
            
            if (mode === 'exam') {
                normalSections.forEach(el => el.classList.add('hidden'));
                examSections.forEach(el => el.classList.add('visible'));
                // Load exam periods when switching to exam mode
                setTimeout(loadExamPeriods, 300);
            } else {
                normalSections.forEach(el => el.classList.remove('hidden'));
                examSections.forEach(el => el.classList.remove('visible'));
                // Load normal mode data when switching back
                setTimeout(() => {
                    loadPeriods();
                    loadHolidays();
                }, 300);
            }
        }
        
        // helper: keep numeric inputs within max digits and optionally auto-advance
        function clampDigits(el, maxLen) {
            if (!el) return;
            let v = (el.value || '').replace(/\D/g, '');
            if (maxLen && v.length > maxLen) v = v.slice(0, maxLen);
            if (el.value !== v) el.value = v;
        }

        function setupAutoAdvance(srcId, maxLen, nextId) {
            const el = document.getElementById(srcId);
            if (!el) return;
            el.addEventListener('input', () => {
                clampDigits(el, maxLen);
                if (el.value.length >= maxLen && nextId) {
                    const nxt = document.getElementById(nextId);
                    if (nxt) nxt.focus();
                }
            });
        }

        function setupMaxLen(srcId, maxLen) {
            const el = document.getElementById(srcId);
            if (!el) return;
            el.addEventListener('input', () => clampDigits(el, maxLen));
        }

        // configure auto-advance for time-setting fields
        window.addEventListener('DOMContentLoaded', () => {
            setupAutoAdvance('setYear', 4, 'setMonth');
            setupAutoAdvance('setMonth', 2, 'setDay');
            setupAutoAdvance('setDay', 2, 'setHour');
            setupAutoAdvance('setHour', 2, 'setMinute');
            setupAutoAdvance('setMinute', 2, 'setSecond');
            setupAutoAdvance('setSecond', 2, null);

            setupAutoAdvance('periodHour', 2, 'periodMinute');
            setupAutoAdvance('periodMinute', 2, 'periodSecond');
            setupAutoAdvance('periodSecond', 2, 'periodDelay');
            setupAutoAdvance('periodDelay', 2, null);
            
            setupAutoAdvance('examPeriodHour', 2, 'examPeriodMinute');
            setupAutoAdvance('examPeriodMinute', 2, 'examPeriodSecond');
            setupAutoAdvance('examPeriodSecond', 2, 'examPeriodDelay');
            setupAutoAdvance('examPeriodDelay', 2, null);

            setupAutoAdvance('holidayYear', 4, 'holidayMonth');
            setupAutoAdvance('holidayMonth', 2, 'holidayDay');
            setupAutoAdvance('holidayDay', 2, null);

            // fields without auto-advance but still need digit limits
            setupMaxLen('manualDelay', 2);
        });

        // Update current time every second
        function updateCurrentTime() {
            fetch('/api/time')
                .then(response => response.json())
                .then(data => {
                    const y = String(data.year).padStart(4, '0');
                    const mo = String(data.month).padStart(2, '0');
                    const d = String(data.day).padStart(2, '0');
                    const h = String(data.hour).padStart(2, '0');
                    const m = String(data.minute).padStart(2, '0');
                    const s = String(data.second).padStart(2, '0');
                    const dow = fullDayNames[data.dayOfWeek];
                    
                    document.getElementById('currentTime').innerText = `${y}-${mo}-${d} ${h}:${m}:${s}`;
                    document.getElementById('currentDayOfWeek').innerText = dow;
                })
                .catch(err => console.error('Error fetching time:', err));
        }
        
        // Get periods data on page load
        function loadPeriods() {
            fetch('/api/periods')
                .then(response => response.json())
                .then(data => {
                    // server already sorts but do a safe client‑side sort as well
                    data.periods.sort((a,b) => {
                        if (a.day !== b.day) return a.day - b.day;
                        if (a.hour !== b.hour) return a.hour - b.hour;
                        if (a.minute !== b.minute) return a.minute - b.minute;
                        return a.second - b.second;
                    });
                    updatePeriodsList(data.periods);
                    updateTimerStatus(data.timerActive);
                })
                .catch(err => console.error('Error loading periods:', err));
        }

        function formatHolidayDate(h) {
            return String(h.year).padStart(4, '0') + '-' +
                   String(h.month).padStart(2, '0') + '-' +
                   String(h.day).padStart(2, '0');
        }

        function loadHolidays() {
            fetch('/api/holidays')
                .then(response => response.json())
                .then(data => {
                    const holidays = (data.holidays || []).slice().sort((a, b) => {
                        if (a.year !== b.year) return a.year - b.year;
                        if (a.month !== b.month) return a.month - b.month;
                        return a.day - b.day;
                    });
                    updateHolidaysList(holidays, !!data.holidayToday, data.maxDates || 15);
                })
                .catch(err => console.error('Error loading holidays:', err));
        }

        function updateHolidaysList(holidays, holidayToday, maxDates) {
            const listDiv = document.getElementById('holidaysList');
            const badge = document.getElementById('holidayTodayBadge');
            if (badge) {
                badge.style.color = holidayToday ? '#d32f2f' : '#2e7d32';
                badge.innerText = holidayToday
                    ? 'Holiday active today: timers are suppressed'
                    : 'No holiday today';
            }

            if (!listDiv) return;
            if (!holidays.length) {
                listDiv.innerHTML = '<div class="status-label">No holidays added yet.</div>';
                return;
            }

            let html = '<div class="status-label" style="margin-bottom: 10px; font-weight: bold;">Holiday Dates (' + holidays.length + '/' + maxDates + '):</div>';
            holidays.forEach((h) => {
                html += '<div style="margin: 8px 0; padding: 8px; background: white; border: 1px solid #ddd; border-radius: 5px; display: flex; justify-content: space-between; align-items: center;">';
                html += '<span style="font-size:16px; font-weight:600;">' + formatHolidayDate(h) + '</span>';
                html += '<button onclick="deleteHoliday(' + h.index + ')" class="delete-btn">Delete</button>';
                html += '</div>';
            });
            listDiv.innerHTML = html;
        }
        
        function addHoliday() {
            const year = parseInt(document.getElementById('holidayYear').value) || 0;
            const month = parseInt(document.getElementById('holidayMonth').value) || 0;
            const day = parseInt(document.getElementById('holidayDay').value) || 0;

            if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
                showError('Invalid holiday date');
                return;
            }

            showInfo('Adding holiday date...');
            fetch('/api/addholiday?year=' + year + '&month=' + month + '&day=' + day)
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('[OK] Holiday date added');
                        document.getElementById('holidayYear').value = '';
                        document.getElementById('holidayMonth').value = '';
                        document.getElementById('holidayDay').value = '';
                        loadHolidays();
                    } else {
                        showError('[ERR] Failed to add holiday: ' + (data.message || 'Unknown error'));
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                })
                .finally(() => {
                    if (btn) {
                        btn.disabled = false;
                        btn.style.opacity = 1;
                        btn.style.cursor = 'pointer';
                    }
                    // Re-apply day enable/disable in case UI state changed.
                    document.getElementById('periodMode').dispatchEvent(new Event('change'));
                });
        }

        function deleteHoliday(index) {
            if (!confirm('Delete this holiday date?')) return;
            fetch('/api/deleteholiday?index=' + index)
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('[OK] Holiday date deleted');
                        loadHolidays();
                    } else {
                        showError('[ERR] Failed to delete holiday');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }

        function clearAllHolidays() {
            if (!confirm('Delete ALL holiday dates? This cannot be undone.')) return;
            showInfo('Clearing all holidays...');
            fetch('/api/clearholidays')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('[OK] All holiday dates cleared');
                        loadHolidays();
                    } else {
                        showError('[ERR] Failed to clear holidays: ' + (data.message || 'Unknown error'));
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Set date & time on RTC
        // helper functions for calendar
        function isLeapYear(y) { return ((y % 4 === 0) && (y % 100 !== 0)) || (y % 400 === 0); }
        function daysInMonth(y, m) {
            if (m < 1 || m > 12) return 0;
            if (m === 2) return isLeapYear(y) ? 29 : 28;
            if ([4,6,9,11].includes(m)) return 30;
            return 31;
        }

        function setDateTime() {
            const year = parseInt(document.getElementById('setYear').value) || new Date().getFullYear();
            const month = parseInt(document.getElementById('setMonth').value) || 1;
            const day = parseInt(document.getElementById('setDay').value) || 1;
            const hour = parseInt(document.getElementById('setHour').value) || 0;
            const minute = parseInt(document.getElementById('setMinute').value) || 0;
            const second = parseInt(document.getElementById('setSecond').value) || 0;
            
            if (month < 1 || month > 12 ||
                day < 1 || day > daysInMonth(year, month) ||
                hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
                showError('Invalid date/time format!');
                return;
            }
            
            showInfo('Setting date & time on RTC...');
            
            fetch('/api/settime?year=' + year + '&month=' + month + '&day=' + day + 
                  '&hour=' + hour + '&minute=' + minute + '&second=' + second)
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Date & Time set successfully on RTC!');
                        // clear inputs
                        document.getElementById('setYear').value = '';
                        document.getElementById('setMonth').value = '';
                        document.getElementById('setDay').value = '';
                        document.getElementById('setHour').value = '';
                        document.getElementById('setMinute').value = '';
                        document.getElementById('setSecond').value = '';
                        setTimeout(updateCurrentTime, 500);
                    } else {
                        showError('✗ Failed to set date/time: ' + (data.message || 'Unknown error'));
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Add time period
        function addPeriod() {
            const btn = document.getElementById('addPeriodBtn');
            if (btn && btn.disabled) return; // guard against double-tap/double-click
            const mode = document.getElementById('periodMode').value;
            const hour = parseInt(document.getElementById('periodHour').value) || 0;
            const minute = parseInt(document.getElementById('periodMinute').value) || 0;
            const second = parseInt(document.getElementById('periodSecond').value) || 0;
            const delaySeconds = parseInt(document.getElementById('periodDelay').value) || 0;
            
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || delaySeconds < 0 || delaySeconds > 60) {
                showError('Invalid period format! Delay must be 0-60 seconds');
                return;
            }
            
            showInfo('Adding time period...');
            
            // build request URL depending on chosen mode
            let url = '/api/addperiod?hour=' + hour + '&minute=' + minute + '&second=' + second + '&delaySeconds=' + delaySeconds;
            if (mode === 'single') {
                // convert UI day (1‑7) to internal 0‑6
            const day = parseInt(document.getElementById('periodDay').value);
            if (isNaN(day) || day < 1 || day > 7) {
                showError('Invalid day selected');
                return;
            }
            url += '&day=' + (day);
            } else {
                url += '&group=' + mode; // server will replicate days
            }

            if (btn) {
                btn.disabled = true;
                btn.style.opacity = 0.7;
                btn.style.cursor = 'not-allowed';
            }

            fetch(url)
              .then(response => response.json())
              .then(data => {
                if (data.success) {
                  showSuccess('✓ ' + (data.message || 'Period(s) added successfully!'));
                  loadPeriods(); // Refresh the list
                  // Clear inputs
                  document.getElementById('periodHour').value = '';
                  document.getElementById('periodMinute').value = '';
                  document.getElementById('periodSecond').value = '';
                  document.getElementById('periodDelay').value = '';
                } else {
                  // show inline error
                  showError('✗ Failed to add period: ' + (data.message || 'Unknown error'));
                  // if server indicates limit reached, show a prominent alert for better UX
                  if (data.message && data.message.toLowerCase().includes('limit')) {
                    alert(data.message);
                  }
                }
              })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                })
                .finally(() => {
                    if (btn) {
                        btn.disabled = false;
                        btn.style.opacity = 1;
                        btn.style.cursor = 'pointer';
                    }
                    document.getElementById('periodMode').dispatchEvent(new Event('change'));
                });
        }
        
        // Function to detect group type based on days pattern
        function detectGroupType(days) {
            days.sort((a, b) => a - b);
            const dayStr = days.join(',');
            
            // Check for known patterns
            if (dayStr === '1,2,3,4,5,6') return 'Mon-Sat';
            if (dayStr === '1,2,3,4,5') return 'Mon-Fri';
            if (dayStr === '1,2,3,4') return 'Mon-Thu';
            if (dayStr === '6') return 'Saturday';
            if (dayStr === '0,1,2,3,4,5,6') return 'Every Day';
            if (dayStr === '0') return 'Sunday';
            if (dayStr === '1') return 'Monday';
            if (dayStr === '2') return 'Tuesday';
            if (dayStr === '3') return 'Wednesday';
            if (dayStr === '4') return 'Thursday';
            if (dayStr === '5') return 'Friday';
            if (dayStr === '6') return 'Saturday';
            
            // For other patterns, show all days
            return days.map(d => fullDayNames[d]).join(', ');
        }

        // Function to group periods by time and delay
        function groupPeriods(periods) {
            const groups = {};
            const processed = new Set();
            
            periods.forEach((period, idx) => {
                if (processed.has(idx)) return;
                
                const key = `${period.hour}:${period.minute}:${period.second}|${period.delaySeconds}`;
                
                if (!groups[key]) {
                    groups[key] = {
                        periods: [],
                        indices: [],
                        time: {hour: period.hour, minute: period.minute, second: period.second},
                        delay: period.delaySeconds
                    };
                }
                
                groups[key].periods.push(period);
                // use server-provided index if available (this is the 'orig' field from API)
                groups[key].indices.push(typeof period.index === 'number' ? period.index : idx);
                processed.add(idx);
            });
            
            // Convert groups to array and sort by time
            return Object.values(groups).sort((a, b) => {
                const aFirst = a.periods[0];
                const bFirst = b.periods[0];
                if (aFirst.day !== bFirst.day) return aFirst.day - bFirst.day;
                if (aFirst.hour !== bFirst.hour) return aFirst.hour - bFirst.hour;
                if (aFirst.minute !== bFirst.minute) return aFirst.minute - bFirst.minute;
                return aFirst.second - bFirst.second;
            });
        }

        // Update periods list display with grouping
        function updatePeriodsList(periods) {
            const listDiv = document.getElementById('periodsList');
            if (periods.length === 0) {
                listDiv.innerHTML = '<div class="status-label">No periods added yet.</div>';
                return;
            }
            
            // Group periods by time and delay
            const groups = groupPeriods(periods);
            
            let html = '<div class="status-label" style="margin-bottom: 10px; font-weight: bold;">Scheduled Periods ({0}):</div>'.replace('{0}', groups.length);
            
            groups.forEach((group) => {
                const timeStr = String(group.time.hour).padStart(2, '0') + ':' + 
                               String(group.time.minute).padStart(2, '0') + ':' + 
                               String(group.time.second).padStart(2, '0');
                const delayMinSec = Math.floor(group.delay / 60) + 'm ' + (group.delay % 60) + 's';
                
                // Get days from all periods in the group
                const days = group.periods.map(p => p.day);
                const groupLabel = detectGroupType(days);
                
                html += '<div style="margin: 8px 0; padding: 8px; background: white; border: 1px solid #ddd; border-radius: 5px; display: flex; justify-content: space-between; align-items: center;">';
                html += '<span><strong>' + groupLabel + '</strong> at ' + timeStr + ' → Duration: ' + delayMinSec + '</span>';
                
                // Pass all indices as a JSON string for grouped operations
                const indicesStr = JSON.stringify(group.indices);
                html += '<button onclick="editGroupPeriod(\'' + indicesStr.replace(/'/g, "\\'") + '\')" style="background: #4caf50; color: white; border: none; border-radius: 3px; padding: 5px 10px; cursor: pointer; font-size: 12px; margin-right:5px;">Edit</button>';
                html += '<button onclick="deleteGroupPeriod(\'' + indicesStr.replace(/'/g, "\\'") + '\')" style="background: #ff6b6b; color: white; border: none; border-radius: 3px; padding: 5px 10px; cursor: pointer; font-size: 12px;">Delete</button>';
                html += '</div>';
            });
            listDiv.innerHTML = html;
        }
        
        // handle mode dropdown to enable/disable day selection
        document.getElementById('periodMode').addEventListener('change', function(e) {
            const mode = e.target.value;
            const dayEl = document.getElementById('periodDay');
            if (mode === 'single') {
                dayEl.disabled = false;
                dayEl.style.opacity = 1;
            } else {
                dayEl.disabled = true;
                dayEl.style.opacity = 0.5;
            }
        });

        // Delete period
        function deletePeriod(index) {
            if (confirm('Delete this period?')) {
                fetch('/api/deleteperiod?index=' + index)
                    .then(response => response.json())
                    .then(data => {
                        if (data.success) {
                            showSuccess('✓ Period deleted!');
                            loadPeriods();
                        } else {
                            showError('✗ Failed to delete period');
                        }
                    })
                    .catch(err => {
                        console.error('Error:', err);
                        showError('Connection error!');
                    });
            }
        }

        // Delete grouped periods (all indices in the group)
        function deleteGroupPeriod(indicesJson) {
            const indices = JSON.parse(indicesJson);
            const msg = indices.length === 1 
                ? 'Delete this period?' 
                : 'Delete all {0} periods in this group?'.replace('{0}', indices.length);
            
            if (confirm(msg)) {
                // Delete in reverse order to maintain correct indices
                const sortedIndices = indices.sort((a, b) => b - a);
                let deletedCount = 0;
                
                const deleteNext = () => {
                    if (deletedCount >= sortedIndices.length) {
                        showSuccess('✓ Period(s) deleted!');
                        loadPeriods();
                        return;
                    }
                    
                    const idx = sortedIndices[deletedCount];
                    fetch('/api/deleteperiod?index=' + idx)
                        .then(response => response.json())
                        .then(data => {
                            if (data.success) {
                                deletedCount++;
                                deleteNext();
                            } else {
                                showError('✗ Failed to delete period at index ' + idx);
                            }
                        })
                        .catch(err => {
                            console.error('Error:', err);
                            showError('Connection error!');
                        });
                };
                
                deleteNext();
            }
        }

        // Clear all periods (UI handler)
        function clearAllPeriods() {
          if (!confirm('Delete ALL scheduled periods? This cannot be undone.')) return;
          showInfo('Clearing all periods...');
          fetch('/api/clearperiods')
            .then(response => response.json())
            .then(data => {
              if (data.success) {
                showSuccess('✓ All periods cleared');
                loadPeriods();
              } else {
                showError('✗ Failed to clear periods: ' + (data.message || 'Unknown error'));
              }
            })
            .catch(err => {
              console.error('Error:', err);
              showError('Connection error!');
            });
        }

        // Edit period: prompts user for new values
        function editPeriod(origIndex) {
            // retrieve current list to find the matching period object
            fetch('/api/periods')
                .then(resp => resp.json())
                .then(data => {
                    const period = data.periods.find(p => p.index === origIndex);
                    if (!period) return;
                    // show 1..7 to the user, convert later
                    let newDay = prompt('Day (1=Sun..7=Sat):', period.day + 1);
                    if (newDay === null) return;
                    newDay = parseInt(newDay);
                    let newHour = prompt('Hour (0-23):', period.hour);
                    if (newHour === null) return;
                    newHour = parseInt(newHour);
                    let newMinute = prompt('Minute (0-59):', period.minute);
                    if (newMinute === null) return;
                    newMinute = parseInt(newMinute);
                    let newSecond = prompt('Second (0-59):', period.second);
                    if (newSecond === null) return;
                    newSecond = parseInt(newSecond);
                    let newDelay = prompt('Delay seconds:', period.delaySeconds);
                    if (newDelay === null) return;
                    newDelay = parseInt(newDelay);

                    // basic validation (day input is 1-7)
                    if (isNaN(newDay) || newDay < 1 || newDay > 7 ||
                        isNaN(newHour) || newHour < 0 || newHour > 23 ||
                        isNaN(newMinute) || newMinute < 0 || newMinute > 59 ||
                        isNaN(newSecond) || newSecond < 0 || newSecond > 59 ||
                        isNaN(newDelay) || newDelay < 0 || newDelay > 60) {
                        showError('Invalid values entered, edit cancelled');
                        return;
                    }

                    fetch('/api/editperiod?index=' + origIndex + '&day=' + (newDay) + '&hour=' + newHour +
                          '&minute=' + newMinute + '&second=' + newSecond + '&delaySeconds=' + newDelay)
                        .then(response => response.json())
                        .then(data => {
                            if (data.success) {
                                showSuccess('✓ Period updated!');
                                loadPeriods();
                            } else {
                                showError('✗ Failed to update period');
                            }
                        })
                        .catch(err => {
                            console.error('Error:', err);
                            showError('Connection error!');
                        });
                });
        }

        // Edit grouped periods (apply changes to all indices in the group)
        function editGroupPeriod(indicesJson) {
            const indices = JSON.parse(indicesJson);
            
            // Fetch current periods to get values from the first period in the group
            fetch('/api/periods')
                .then(resp => resp.json())
                .then(data => {
                    const firstPeriod = data.periods.find(p => p.index === indices[0]);
                    if (!firstPeriod) return;
                    
                    // Show prompts for new values (NO day editing for groups - keep original day pattern)
                    let newHour = prompt('Hour (0-23):', firstPeriod.hour);
                    if (newHour === null) return;
                    newHour = parseInt(newHour);
                    let newMinute = prompt('Minute (0-59):', firstPeriod.minute);
                    if (newMinute === null) return;
                    newMinute = parseInt(newMinute);
                    let newSecond = prompt('Second (0-59):', firstPeriod.second);
                    if (newSecond === null) return;
                    newSecond = parseInt(newSecond);
                    let newDelay = prompt('Delay seconds:', firstPeriod.delaySeconds);
                    if (newDelay === null) return;
                    newDelay = parseInt(newDelay);

                    // Validate input
                    if (isNaN(newHour) || newHour < 0 || newHour > 23 ||
                        isNaN(newMinute) || newMinute < 0 || newMinute > 59 ||
                        isNaN(newSecond) || newSecond < 0 || newSecond > 59 ||
                        isNaN(newDelay) || newDelay < 0 || newDelay > 60) {
                        showError('Invalid values entered, edit cancelled');
                        return;
                    }

                    // Update all periods in the group with the same new values (keep original day for each)
                    let updatedCount = 0;
                    
                    const updateNext = () => {
                        if (updatedCount >= indices.length) {
                            showSuccess('✓ All periods updated!');
                            loadPeriods();
                            return;
                        }
                        
                        const idx = indices[updatedCount];
                        // Get the original day for this period
                        const period = data.periods.find(p => p.index === idx);
                        const originalDay = period ? period.day : firstPeriod.day;
                        
                        // Edit without changing the day - convert from 0-6 to 1-7 for the API
                        fetch('/api/editperiod?index=' + idx + '&day=' + (originalDay + 1) + '&hour=' + newHour +
                              '&minute=' + newMinute + '&second=' + newSecond + '&delaySeconds=' + newDelay)
                            .then(response => response.json())
                            .then(data => {
                                if (data.success) {
                                    updatedCount++;
                                    updateNext();
                                } else {
                                    showError('✗ Failed to update period at index ' + idx);
                                }
                            })
                            .catch(err => {
                                console.error('Error:', err);
                                showError('Connection error!');
                            });
                    };
                    
                    updateNext();
                });
        }

        // Manual trigger handler
        function triggerNow() {
            const d = parseInt(document.getElementById('manualDelay').value) || 0;
            if (d < 0 || d > 60) {
                showError('Enter a valid delay 0-60 seconds');
                return;
            }
            showInfo('Triggering output...');
            fetch('/api/trigger?delaySeconds=' + d)
                .then(resp => resp.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Output triggered for ' + d + 's');
                        document.getElementById('manualDelay').value = '';
                    } else {
                        showError('✗ Trigger failed');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Start timer
        function startTimer() {
            showInfo('Starting timer...');
            
            fetch('/api/starttimer')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Timer started!');
                        updateTimerStatus(true);
                    } else {
                        showError('✗ Failed to start timer');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Stop timer
        function stopTimer() {
            fetch('/api/stoptimer')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Timer stopped!');
                        updateTimerStatus(false);
                    } else {
                        showError('✗ Failed to stop timer');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // ===== EXAM MODE FUNCTIONS =====
        
        // Load exam periods
        function loadExamPeriods() {
            fetch('/api/examperiods')
                .then(response => response.json())
                .then(data => {
                    // server already sorts but do a safe client-side sort as well
                    data.periods.sort((a,b) => {
                        // choose comparison key as dayStart for groups or day otherwise
                        const da = (a.isGroup ? a.dayStart : a.day);
                        const db = (b.isGroup ? b.dayStart : b.day);
                        if (da !== db) return da - db;
                        if (a.hour !== b.hour) return a.hour - b.hour;
                        if (a.minute !== b.minute) return a.minute - b.minute;
                        return a.second - b.second;
                    });
                    updateExamPeriodsList(data.periods);
                    updateExamTimerStatus(data.timerActive);
                })
                .catch(err => console.error('Error loading exam periods:', err));
        }
        
        // Add exam period (only Mon-Fri or Mon-Sat)
        function addExamPeriod() {
            const mode = document.getElementById('examPeriodMode').value;
            const hour = parseInt(document.getElementById('examPeriodHour').value) || 0;
            const minute = parseInt(document.getElementById('examPeriodMinute').value) || 0;
            const second = parseInt(document.getElementById('examPeriodSecond').value) || 0;
            const delaySeconds = parseInt(document.getElementById('examPeriodDelay').value) || 0;
            
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || delaySeconds < 0 || delaySeconds > 60) {
                showError('Invalid period format! Delay must be 0-60 seconds');
                return;
            }
            
            showInfo('Adding exam schedule...');
            
            let url = '/api/addexamperiod?hour=' + hour + '&minute=' + minute + '&second=' + second + '&delaySeconds=' + delaySeconds;
            let lastPeriodMode = document.getElementById("examPeriodMode").value;
            if (mode === 'weekdays') {
                url += '&group=weekdays'; // Mon-Fri
            } else if (mode === 'monfri') {
                url += '&group=monfri';   // Mon-Sat
            }

            fetch(url)
              .then(response => response.json())
              .then(data => {
                if (data.success) {
                  showSuccess('✓ Exam schedule added successfully!');
                  loadExamPeriods(); // Refresh the list
                  // Clear inputsloadExamPeriods(); // Refresh the list
                  document.getElementById('examPeriodHour').value = '';
                  document.getElementById('examPeriodMinute').value = '';
                  document.getElementById('examPeriodSecond').value = '';
                  document.getElementById('examPeriodDelay').value = '';
                  document.getElementById('examPeriodMode').value = lastPeriodMode;
                } else {
                  showError('✗ Failed to add exam schedule: ' + (data.message || 'Unknown error'));
                }
              })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Update exam periods list display using normal-mode grouping
        function updateExamPeriodsList(periods) {
            const listDiv = document.getElementById('examPeriodsList');
            if (periods.length === 0) {
                listDiv.innerHTML = '<div class="status-label">No exam schedules added yet.</div>';
                return;
            }
            
            const groups = groupPeriods(periods);
            let html = '<div class="status-label" style="margin-bottom: 10px; font-weight: bold;">Exam Schedules ({0}):</div>'.replace('{0}', groups.length);
            groups.forEach((group) => {
                const timeStr = String(group.time.hour).padStart(2, '0') + ':' + 
                               String(group.time.minute).padStart(2, '0') + ':' + 
                               String(group.time.second).padStart(2, '0');
                const delayMinSec = Math.floor(group.delay / 60) + 'm ' + (group.delay % 60) + 's';
                const days = group.periods.map(p => p.day);
                const groupLabel = detectGroupType(days);
                
                html += '<div style="margin: 8px 0; padding: 8px; background: white; border: 1px solid #ddd; border-radius: 5px; display: flex; justify-content: space-between; align-items: center;">';
                html += '<span><strong>' + groupLabel + '</strong> at ' + timeStr + ' → Duration: ' + delayMinSec + '</span>';
                const indicesStr = JSON.stringify(group.indices);
                html += '<button onclick="editGroupExamPeriod(\'' + indicesStr.replace(/'/g, "\\'") + '\')" style="background: #4caf50; color: white; border: none; border-radius: 3px; padding: 5px 10px; cursor: pointer; font-size: 12px; margin-right:5px;">Edit</button>';
                html += '<button onclick="deleteGroupExamPeriod(\'' + indicesStr.replace(/'/g, "\\'") + '\')" style="background: #ff6b6b; color: white; border: none; border-radius: 3px; padding: 5px 10px; cursor: pointer; font-size: 12px;">Delete</button>';
                html += '</div>';
            });
            listDiv.innerHTML = html;
        }
        
        // Delete a single exam period by index
        function deleteExamPeriod(index) {
            if (confirm('Delete this exam schedule?')) {
                fetch('/api/deleteexamperiod?index=' + index)
                    .then(response => response.json())
                    .then(data => {
                        if (data.success) {
                            showSuccess('✓ Exam schedule deleted!');
                            loadExamPeriods();
                        } else {
                            showError('✗ Failed to delete exam schedule');
                        }
                    })
                    .catch(err => {
                        console.error('Error:', err);
                        showError('Connection error!');
                    });
            }
        }
        
        // Edit grouped exam periods (apply changes to entire range in one request)
        function editGroupExamPeriod(indicesJson) {
            const indices = JSON.parse(indicesJson);
            fetch('/api/examperiods')
                .then(resp => resp.json())
                .then(data => {
                    // pick base period from first index
                    const basePeriod = data.periods.find(p => p.index === indices[0]);
                    if (!basePeriod) return;
                    // determine day range from all indices
                    const days = indices.map(i => {
                        const p = data.periods.find(x => x.index === i);
                        return p ? p.day : basePeriod.day;
                    }).sort((a,b)=>a-b);
                    const dayStart = days[0];
                    const dayEnd = days[days.length-1];

                    let newHour = prompt('Hour (0-23):', basePeriod.hour);
                    if (newHour === null) return;
                    newHour = parseInt(newHour);
                    let newMinute = prompt('Minute (0-59):', basePeriod.minute);
                    if (newMinute === null) return;
                    newMinute = parseInt(newMinute);
                    let newSecond = prompt('Second (0-59):', basePeriod.second);
                    if (newSecond === null) return;
                    newSecond = parseInt(newSecond);
                    let newDelay = prompt('Delay seconds:', basePeriod.delaySeconds);
                    if (newDelay === null) return;
                    newDelay = parseInt(newDelay);
                    if (isNaN(newHour) || newHour < 0 || newHour > 23 ||
                        isNaN(newMinute) || newMinute < 0 || newMinute > 59 ||
                        isNaN(newSecond) || newSecond < 0 || newSecond > 59 ||
                        isNaN(newDelay) || newDelay < 0 || newDelay > 60) {
                        showError('Invalid values entered, edit cancelled');
                        return;
                    }

                    let url = '/api/editexamperiod?index=' + indices[0] +
                              '&dayStart=' + (dayStart + 1) +
                              '&dayEnd=' + (dayEnd + 1) +
                              '&hour=' + newHour + '&minute=' + newMinute +
                              '&second=' + newSecond + '&delaySeconds=' + newDelay;
                    fetch(url)
                        .then(resp => resp.json())
                        .then(d => {
                            if (d.success) showSuccess('✓ Exam schedule updated');
                            else showError('✗ Failed to update exam schedule: ' + (d.message || 'Unknown error'));
                            loadExamPeriods();
                        })
                        .catch(err => { console.error('Error:', err); showError('Connection error!'); });
                })
                .catch(err => { console.error('Error fetching periods:', err); });
        }

        // Delete grouped exam periods (indices JSON string)
        function deleteGroupExamPeriod(indicesJson) {
            let indices = JSON.parse(indicesJson);
            // sort descending so removals don't shift remaining indexes
            indices.sort((a,b) => b - a);
            if (!confirm('Delete this exam schedule?')) return;
            let done = 0;
            const delNext = () => {
                if (done >= indices.length) {
                    showSuccess('✓ Exam schedule deleted');
                    loadExamPeriods();
                    return;
                }
                fetch('/api/deleteexamperiod?index=' + indices[done++])
                    .then(resp => resp.json())
                    .then(d => delNext())
                    .catch(err => { console.error('Error:', err); showError('Connection error!'); });
            };
            delNext();
        }

        // Clear all exam periods
        function clearAllExamPeriods() {
          if (!confirm('Delete ALL exam schedules? This cannot be undone.')) return;
          showInfo('Clearing all exam schedules...');
          fetch('/api/clearexamperiods')
            .then(response => response.json())
            .then(data => {
              if (data.success) {
                showSuccess('✓ All exam schedules cleared');
                loadExamPeriods();
              } else {
                showError('✗ Failed to clear exam schedules: ' + (data.message || 'Unknown error'));
              }
            })
            .catch(err => {
              console.error('Error:', err);
              showError('Connection error!');
            });
        }
        
        // Start exam timer
        function startExamTimer() {
            showInfo('Starting exam timer...');
            
            fetch('/api/startexamtimer')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Exam timer started!');
                        updateExamTimerStatus(true);
                    } else {
                        showError('✗ Failed to start exam timer');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Stop exam timer
        function stopExamTimer() {
            fetch('/api/stopexamtimer')
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        showSuccess('✓ Exam timer stopped!');
                        updateExamTimerStatus(false);
                    } else {
                        showError('✗ Failed to stop exam timer');
                    }
                })
                .catch(err => {
                    console.error('Error:', err);
                    showError('Connection error!');
                });
        }
        
        // Update exam timer status display
        function updateExamTimerStatus(isActive) {
            const statusDiv = document.getElementById('examTimerStatus');
            if (statusDiv) {
                if (isActive) {
                    statusDiv.className = 'timer-status status-active';
                    statusDiv.innerHTML = '<span class="loading"></span>Exam Timer is ACTIVE';
                } else {
                    statusDiv.className = 'timer-status status-inactive';
                    statusDiv.innerHTML = '⭕ Exam Timer is INACTIVE';
                }
            }
        }
        
        // Update timer status display
        function updateTimerStatus(isActive) {
            const statusDiv = document.getElementById('timerStatus');
            if (isActive) {
                statusDiv.className = 'timer-status status-active';
                statusDiv.innerHTML = '<span class="loading"></span>Timer is ACTIVE';
            } else {
                statusDiv.className = 'timer-status status-inactive';
                statusDiv.innerHTML = '⭕ Timer is INACTIVE';
            }
        }
        
        // Show messages
        function showError(msg) {
            const el = document.getElementById('errorMessage');
            el.innerText = msg;
            el.style.display = 'block';
            document.getElementById('successMessage').style.display = 'none';
            setTimeout(() => { el.style.display = 'none'; }, 5000);
        }
        
        function showSuccess(msg) {
            const el = document.getElementById('successMessage');
            el.innerText = msg;
            el.style.display = 'block';
            document.getElementById('errorMessage').style.display = 'none';
            setTimeout(() => { el.style.display = 'none'; }, 5000);
        }
        
        function showInfo(msg) {
            const el = document.getElementById('successMessage');
            el.innerText = msg;
            el.style.display = 'block';
            document.getElementById('errorMessage').style.display = 'none';
        }
        
        // Initialize on page load
        window.addEventListener('DOMContentLoaded', function() {
            updateCurrentTime();
            loadPeriods();
            loadHolidays();
            
            // make sure the day dropdown state matches the default mode
            document.getElementById('periodMode').dispatchEvent(new Event('change'));            

            // Update time every second
            setInterval(updateCurrentTime, 1000);
            setInterval(loadHolidays, 60000);
            
            // Refresh timer status every 2 seconds
            setInterval(() => {
                fetch('/api/periods')
                    .then(response => response.json())
                    .then(data => {
                        updateTimerStatus(data.timerActive);
                    })
                    .catch(err => console.error('Error:', err));
            }, 2000);
        });
        
        // Format input to always be 2 digits
        document.getElementById('setHour').addEventListener('change', formatInput);
        document.getElementById('setMinute').addEventListener('change', formatInput);
        document.getElementById('setSecond').addEventListener('change', formatInput);
        
        function formatInput(e) {
            let val = parseInt(e.target.value) || 0;
            e.target.value = String(val).padStart(2, '0');
        }
    </script>
</body>
</html>
)rawliteral";
  return html;
}

void handleGetTime() {
  DateTime now = rtcAvailable ? rtc.now() : DateTime(2000,1,1,0,0,0);
  String json = "{";
  json += "\"year\":" + String(now.year()) + ",";
  json += "\"month\":" + String(now.month()) + ",";
  json += "\"day\":" + String(now.day()) + ",";
  json += "\"dayOfWeek\":" + String(now.dayOfTheWeek()) + ",";
  json += "\"hour\":" + String(now.hour()) + ",";
  json += "\"minute\":" + String(now.minute()) + ",";
  json += "\"second\":" + String(now.second());
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetTime() {
  if (!rtcAvailable) {
    String response = "{\"success\":false,\"message\":\"RTC not available\"}";
    server.send(503, "application/json", response);
    return;
  }
  
  if (server.hasArg("year") && server.hasArg("month") && server.hasArg("day") &&
      server.hasArg("hour") && server.hasArg("minute") && server.hasArg("second")) {
    uint16_t year = server.arg("year").toInt();
    int month = server.arg("month").toInt();
    int day = server.arg("day").toInt();
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int second = server.arg("second").toInt();
    
    // Validate inputs (negative values caught too)
    if (month < 1 || month > 12 ||
        day < 1 || day > daysInMonth(year, month) ||
        !isValidTime(hour, minute, second)) {
      String response = "{\"success\":false,\"message\":\"Invalid date/time values\"}";
      server.send(400, "application/json", response);
      return;
    }
    
    // safe to cast down now
    rtc.adjust(DateTime(year, month, day, (uint8_t)hour, (uint8_t)minute, (uint8_t)second));
    ////Serial.print.printf("RTC time set to: %04d-%02d-%02d %02d:%02d:%02d\n", year, month, day, hour, minute, second);
    
    String response = "{\"success\":true}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleGetPeriods() {
  // return a sorted copy of the periods along with their original indices
  struct Indexed { Period p; int orig; } arr[MAX_PERIODS];
  int count = timerData.numPeriods;

  // copy with index
  for (int i = 0; i < count; i++) {
    arr[i].p = timerData.periods[i];
    arr[i].orig = i;
  }
  // bubble sort by day/time
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      Period &a = arr[i].p;
      Period &b = arr[j].p;
      bool swap = false;
      if (b.day < a.day) swap = true;
      else if (b.day == a.day) {
        if (b.hour < a.hour) swap = true;
        else if (b.hour == a.hour) {
          if (b.minute < a.minute) swap = true;
          else if (b.minute == a.minute && b.second < a.second) swap = true;
        }
      }
      if (swap) {
        Indexed tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
    }
  }

  String json = "{";
  json += "\"periods\": [";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"index\":" + String(arr[i].orig) + ",";
    json += "\"day\":" + String(arr[i].p.day) + ",";
    json += "\"hour\":" + String(arr[i].p.hour) + ",";
    json += "\"minute\":" + String(arr[i].p.minute) + ",";
    json += "\"second\":" + String(arr[i].p.second) + ",";
    json += "\"delaySeconds\":" + String(arr[i].p.delaySeconds);
    json += "}";
  }
  json += "],";
  json += "\"timerActive\":" + String(timerData.timerActive ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleGetHolidays() {
  String json = "{";
  json += "\"maxDates\":" + String(HOLIDAY_MAX_DATES) + ",";
  json += "\"holidays\":[";
  for (int i = 0; i < holidayData.numDates; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"index\":" + String(i) + ",";
    json += "\"year\":" + String(holidayData.dates[i].year) + ",";
    json += "\"month\":" + String(holidayData.dates[i].month) + ",";
    json += "\"day\":" + String(holidayData.dates[i].day);
    json += "}";
  }
  json += "]";
  if (rtcAvailable) {
    json += ",\"holidayToday\":" + String(isHolidayToday(rtc.now()) ? "true" : "false");
  } else {
    json += ",\"holidayToday\":false";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleAddPeriod() {
  // hour/minute/second/delay are required, plus either a day or a group flag
  if (server.hasArg("hour") && server.hasArg("minute") && server.hasArg("second") && server.hasArg("delaySeconds") &&
      (server.hasArg("day") || server.hasArg("group"))) {
    // parse and validate the numeric fields first
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int second = server.arg("second").toInt();
    int delay = server.arg("delaySeconds").toInt();

    if (!isValidTime(hour, minute, second) || !isValidDelay(delay)) {
      String response = "{\"success\":false,\"message\":\"Invalid time/delay values\"}";
      server.send(400, "application/json", response);
      return;
    }

    // determine which days should be created (normalize 1-7 → 0-6)
    uint8_t daysArr[7];
    int dayCount = 0;

    if (server.hasArg("group")) {
      String grp = server.arg("group");
      if (grp == "weekdays") {
        for (uint8_t d = 1; d <= 5; d++) daysArr[dayCount++] = d;
      } else if (grp == "mon_to_thu") {
        for (uint8_t d = 1; d <= 4; d++) daysArr[dayCount++] = d;
      } else if (grp == "saturday") {
        daysArr[dayCount++] = 6;
      } else if (grp == "everyday") {
        for (uint8_t d = 0; d < 7; d++) daysArr[dayCount++] = d;
      } else if (grp == "single" && server.hasArg("day")) {
        int nd = normalizeDay(server.arg("day").toInt());
        if (nd >= 0) daysArr[dayCount++] = nd;
      }
    } else {
      int nd = normalizeDay(server.arg("day").toInt());
      if (nd >= 0) daysArr[dayCount++] = nd;
    }

    if (dayCount == 0) {
      String response = "{\"success\":false,\"message\":\"Invalid group/day\"}";
      server.send(400, "application/json", response);
      return;
    }

    // Count actual additions after de-duplication (prevents accidental repeats).
    int wouldAdd = 0;
    for (int i = 0; i < dayCount; i++) {
      Period candidate;
      candidate.day = daysArr[i];
      candidate.hour = (uint8_t)hour;
      candidate.minute = (uint8_t)minute;
      candidate.second = (uint8_t)second;
      candidate.delaySeconds = (uint16_t)delay;
      if (!hasDuplicateInTimerData(candidate, timerData.numPeriods)) wouldAdd++;
    }

    if (wouldAdd <= 0) {
      server.send(200, "application/json", "{\"success\":true,\"added\":0,\"skipped\":1,\"message\":\"Already exists\"}");
      return;
    }

    if ((int)timerData.numPeriods + wouldAdd > MAX_PERIODS) {
      String response = "{\"success\":false,\"message\":\"Not enough space for selected days (limit " + String(MAX_PERIODS) + ")\"}";
      server.send(400, "application/json", response);
      return;
    }

    int added = 0;
    int skipped = 0;
    for (int i = 0; i < dayCount; i++) {
      Period newPeriod;
      newPeriod.day = daysArr[i];
      newPeriod.hour = (uint8_t)hour;
      newPeriod.minute = (uint8_t)minute;
      newPeriod.second = (uint8_t)second;
      newPeriod.delaySeconds = (uint16_t)delay;

      if (hasDuplicateInTimerData(newPeriod, timerData.numPeriods)) {
        skipped++;
        continue;
      }
      timerData.periods[timerData.numPeriods++] = newPeriod;
      added++;
    }

    saveDataToPreferences();

    String response = "{\"success\":true,\"added\":" + String(added) + ",\"skipped\":" + String(skipped) + ",\"message\":\"Added " + String(added) + " period(s)\"}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleAddHoliday() {
  if (!(server.hasArg("year") && server.hasArg("month") && server.hasArg("day"))) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing parameters\"}");
    return;
  }

  int year = server.arg("year").toInt();
  int month = server.arg("month").toInt();
  int day = server.arg("day").toInt();

  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid date\"}");
    return;
  }

  for (int i = 0; i < holidayData.numDates; i++) {
    const HolidayDate &h = holidayData.dates[i];
    if (h.year == year && h.month == month && h.day == day) {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"Date already exists\"}");
      return;
    }
  }

  if (holidayData.numDates >= HOLIDAY_MAX_DATES) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Holiday limit reached (15)\"}");
    return;
  }

  HolidayDate hd;
  hd.year = (uint16_t)year;
  hd.month = (uint8_t)month;
  hd.day = (uint8_t)day;
  holidayData.dates[holidayData.numDates++] = hd;
  saveDataToPreferences();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleDeletePeriod() {
  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < timerData.numPeriods) {
      for (int i = index; i < timerData.numPeriods - 1; i++) {
        timerData.periods[i] = timerData.periods[i + 1];
      }
      timerData.numPeriods--;
      saveDataToPreferences();
      String response = "{\"success\":true}";
      server.send(200, "application/json", response);
    } else {
      String response = "{\"success\":false,\"message\":\"Invalid index\"}";
      server.send(400, "application/json", response);
    }
  } else {
    String response = "{\"success\":false,\"message\":\"Missing index\"}";
    server.send(400, "application/json", response);
  }
}

void handleDeleteHoliday() {
  if (!server.hasArg("index")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing index\"}");
    return;
  }

  int index = server.arg("index").toInt();
  if (index < 0 || index >= holidayData.numDates) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid index\"}");
    return;
  }

  for (int i = index; i < holidayData.numDates - 1; i++) {
    holidayData.dates[i] = holidayData.dates[i + 1];
  }
  holidayData.numDates--;
  saveDataToPreferences();
  server.send(200, "application/json", "{\"success\":true}");
}

// clear all scheduled periods
void handleClearPeriods() {
  // reset in-memory
  timerData.numPeriods = 0;
  timerData.timerActive = false;
  saveDataToPreferences();

  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleClearHolidays() {
  holidayData.numDates = 0;
  saveDataToPreferences();
  server.send(200, "application/json", "{\"success\":true}");
}

// handle editing an existing period
void handleEditPeriod() {
  if (server.hasArg("index") && server.hasArg("day") && server.hasArg("hour") &&
      server.hasArg("minute") && server.hasArg("second") && server.hasArg("delaySeconds")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < timerData.numPeriods) {
      int nd = normalizeDay(server.arg("day").toInt());
      int hour = server.arg("hour").toInt();
      int minute = server.arg("minute").toInt();
      int second = server.arg("second").toInt();
      int delay = server.arg("delaySeconds").toInt();

      if (nd < 0 || !isValidTime(hour, minute, second) || !isValidDelay(delay)) {
        String response = "{\"success\":false,\"message\":\"Invalid parameter values\"}";
        server.send(400, "application/json", response);
        return;
      }

      timerData.periods[index].day = nd;
      timerData.periods[index].hour = hour;
      timerData.periods[index].minute = minute;
      timerData.periods[index].second = second;
      timerData.periods[index].delaySeconds = delay;
      saveDataToPreferences();
      String response = "{\"success\":true}";
      server.send(200, "application/json", response);
    } else {
      String response = "{\"success\":false,\"message\":\"Invalid index\"}";
      server.send(400, "application/json", response);
    }
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

// manual trigger endpoint
void handleTrigger() {
  if (server.hasArg("delaySeconds")) {
    int d = server.arg("delaySeconds").toInt();
    if (!isValidDelay(d)) {
      String response = "{\"success\":false,\"message\":\"Invalid delay\"}";
      server.send(400, "application/json", response);
      return;
    }
    ////Serial.print.printf("[API] manual trigger for %d seconds\n", d);
    triggerTimer(d);
    String response = "{\"success\":true}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing delaySeconds\"}";
    server.send(400, "application/json", response);
  }
}

void handleStartTimer() {
  timerData.timerActive = true;
  saveDataToPreferences();
  
  ////Serial.print.println("Timer started");
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleStopTimer() {
  timerData.timerActive = false;
  
  // Force relay off immediately
  digitalWrite(TIMER_OUTPUT_PIN, LOW);
  relayState.isActive = false;
  
  saveDataToPreferences();
  
  ////Serial.print.println("Timer stopped");
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

// ==================== EXAM MODE HANDLERS ====================

void handleSetExamMode() {
  if (server.hasArg("mode")) {
    String modeStr = server.arg("mode");
    bool newExamMode = (modeStr == "true") || (modeStr == "1");
    
    // When switching to exam mode, stop normal timer
    if (newExamMode && timerData.timerActive) {
      timerData.timerActive = false;
      digitalWrite(TIMER_OUTPUT_PIN, LOW);
      relayState.isActive = false;
    }
    
    // When switching to normal mode, stop exam timer
    if (!newExamMode && examData.timerActive) {
      examData.timerActive = false;
      digitalWrite(TIMER_OUTPUT_PIN, LOW);
      relayState.isActive = false;
    }
    
    examModeEnabled = newExamMode;
    saveDataToPreferences();
    
    String response = "{\"success\":true}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing mode parameter\"}";
    server.send(400, "application/json", response);
  }
}

void handleGetExamPeriods() {
  // return a sorted copy of the exam periods along with their original indices
  struct Indexed { Period p; int orig; } arr[MAX_PERIODS];
  int count = examData.numPeriods;

  // copy with index
  for (int i = 0; i < count; i++) {
    arr[i].p = examData.periods[i];
    arr[i].orig = i;
  }
  // bubble sort by day/time
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      Period &a = arr[i].p;
      Period &b = arr[j].p;
      bool swap = false;
      if (b.day < a.day) swap = true;
      else if (b.day == a.day) {
        if (b.hour < a.hour) swap = true;
        else if (b.hour == a.hour) {
          if (b.minute < a.minute) swap = true;
          else if (b.minute == a.minute && b.second < a.second) swap = true;
        }
      }
      if (swap) {
        Indexed tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
    }
  }

  String json = "{";
  json += "\"periods\": [";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"index\":" + String(arr[i].orig) + ",";
    json += "\"day\":" + String(arr[i].p.day) + ",";
    json += "\"hour\":" + String(arr[i].p.hour) + ",";
    json += "\"minute\":" + String(arr[i].p.minute) + ",";
    json += "\"second\":" + String(arr[i].p.second) + ",";
    json += "\"delaySeconds\":" + String(arr[i].p.delaySeconds);
    json += "}";
  }
  json += "],";
  json += "\"timerActive\":" + String(examData.timerActive ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleAddExamPeriod() {
  // Similar to normal period, but only supports weekdays and saturday (1-5 and 1-6)
  if (server.hasArg("hour") && server.hasArg("minute") && server.hasArg("second") && server.hasArg("delaySeconds") &&
      server.hasArg("group")) {
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int second = server.arg("second").toInt();
    int delay = server.arg("delaySeconds").toInt();

    if (!isValidTime(hour, minute, second) || !isValidDelay(delay)) {
      String response = "{\"success\":false,\"message\":\"Invalid time/delay values\"}";
      server.send(400, "application/json", response);
      return;
    }

    uint8_t daysArr[7];
    int dayCount = 0;
    String grp = server.arg("group");

    if (grp == "weekdays") {
      // Monday to Friday (1-5)
      for (uint8_t d = 1; d <= 5; d++) daysArr[dayCount++] = d;
    } else if (grp == "monfri") {
      // Monday to Saturday (1-6)
      for (uint8_t d = 1; d <= 6; d++) daysArr[dayCount++] = d;
    } else {
      String response = "{\"success\":false,\"message\":\"Invalid group for exam mode\"}";
      server.send(400, "application/json", response);
      return;
    }

    if (examData.numPeriods + dayCount > EXAM_MAX_PERIODS) {
      String response = "{\"success\":false,\"message\":\"Not enough space for all selected days (limit " + String(EXAM_MAX_PERIODS) + ")\"}";
      server.send(400, "application/json", response);
      return;
    }

    for (int i = 0; i < dayCount; i++) {
      Period newPeriod;
      newPeriod.day = daysArr[i];
      newPeriod.hour = hour;
      newPeriod.minute = minute;
      newPeriod.second = second;
      newPeriod.delaySeconds = delay;
      examData.periods[examData.numPeriods++] = newPeriod;
    }

    saveDataToPreferences();

    String response = "{\"success\":true}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleDeleteExamPeriod() {
  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < examData.numPeriods) {
      // Determine if caller wants to delete a group range
      if (server.hasArg("dayStart") && server.hasArg("dayEnd")) {
        int ds = normalizeDay(server.arg("dayStart").toInt());
        int de = normalizeDay(server.arg("dayEnd").toInt());
        if (ds < 0 || de < 0 || de < ds) {
          String response = "{\"success\":false,\"message\":\"Invalid day range\"}";
          server.send(400, "application/json", response);
          return;
        }
        // remove all periods that fall within [ds,de] and match the time/delay of the indexed period
        Period base = examData.periods[index];
        Period tmp[MAX_PERIODS];
        int newCount = 0;
        for (int i = 0; i < examData.numPeriods; i++) {
          Period &p = examData.periods[i];
          if (p.day >= ds && p.day <= de && p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
            // skip (delete)
            continue;
          }
          tmp[newCount++] = p;
        }
        // copy back
        for (int i = 0; i < newCount; i++) examData.periods[i] = tmp[i];
        examData.numPeriods = newCount;
        saveDataToPreferences();
        String response = "{\"success\":true}";
        server.send(200, "application/json", response);
        return;
      }

      // No explicit range provided — attempt to detect contiguous group with identical time/delay
      Period base = examData.periods[index];
      bool present[7] = {false,false,false,false,false,false,false};
      for (int i = 0; i < examData.numPeriods; i++) {
        Period &p = examData.periods[i];
        if (p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
          if (p.day >=0 && p.day <=6) present[p.day] = true;
        }
      }
      // find consecutive range containing base.day
      int start = base.day, end = base.day;
      while (start > 0 && present[start-1]) start--;
      while (end < 6 && present[end+1]) end++;
      if (end - start >= 1) {
        // delete all in range [start,end] that match base time/delay
        Period tmp[MAX_PERIODS];
        int newCount = 0;
        for (int i = 0; i < examData.numPeriods; i++) {
          Period &p = examData.periods[i];
          if (p.day >= start && p.day <= end && p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
            continue;
          }
          tmp[newCount++] = p;
        }
        for (int i = 0; i < newCount; i++) examData.periods[i] = tmp[i];
        examData.numPeriods = newCount;
        saveDataToPreferences();
        String response = "{\"success\":true}";
        server.send(200, "application/json", response);
        return;
      }

      // Otherwise delete single index entry (shift down)
      for (int i = index; i < examData.numPeriods - 1; i++) {
        examData.periods[i] = examData.periods[i + 1];
      }
      examData.numPeriods--;
      saveDataToPreferences();
      String response = "{\"success\":true}";
      server.send(200, "application/json", response);
    } else {
      String response = "{\"success\":false,\"message\":\"Invalid index\"}";
      server.send(400, "application/json", response);
    }
  } else {
    String response = "{\"success\":false,\"message\":\"Missing index\"}";
    server.send(400, "application/json", response);
  }
}

void handleClearExamPeriods() {
  examData.numPeriods = 0;
  examData.timerActive = false;

  // clear in-memory slots and EEPROM area used for exam periods
  for (int i = 0; i < EXAM_MAX_PERIODS; i++) {
    examData.periods[i].day = 0;
    examData.periods[i].hour = 0;
    examData.periods[i].minute = 0;
    examData.periods[i].second = 0;
    examData.periods[i].delaySeconds = 0;
  }
  saveDataToPreferences();

  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleEditExamPeriod() {
  if (server.hasArg("index") && server.hasArg("hour") &&
      server.hasArg("minute") && server.hasArg("second") && server.hasArg("delaySeconds")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < examData.numPeriods) {
      bool hasDay = server.hasArg("day");
      bool hasRange = server.hasArg("dayStart") && server.hasArg("dayEnd");
      int nd = hasDay ? normalizeDay(server.arg("day").toInt()) : -1;
      int hour = server.arg("hour").toInt();
      int minute = server.arg("minute").toInt();
      int second = server.arg("second").toInt();
      int delay = server.arg("delaySeconds").toInt();

      if ((!hasDay && !hasRange) || (hasDay && nd < 0) ||
          !isValidTime(hour, minute, second) || !isValidDelay(delay)) {
        String response = "{\"success\":false,\"message\":\"Invalid parameter values\"}";
        server.send(400, "application/json", response);
        return;
      }

      // If caller provided a day range, apply update to entire range
      if (hasRange) {
        int ds = normalizeDay(server.arg("dayStart").toInt());
        int de = normalizeDay(server.arg("dayEnd").toInt());
        if (ds < 0 || de < 0 || de < ds) {
          String response = "{\"success\":false,\"message\":\"Invalid day range\"}";
          server.send(400, "application/json", response);
          return;
        }
        // Update existing entries within range that match original time/delay; if missing, add new entries
        Period base = examData.periods[index];
        bool foundForDay[7] = {false,false,false,false,false,false,false};
        for (int i = 0; i < examData.numPeriods; i++) {
          Period &p = examData.periods[i];
          if (p.day >= ds && p.day <= de && p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
            p.hour = hour;
            p.minute = minute;
            p.second = second;
            p.delaySeconds = delay;
            foundForDay[p.day] = true;
          }
        }
        // add missing days
        for (int d = ds; d <= de; d++) {
          if (!foundForDay[d]) {
            if (examData.numPeriods < EXAM_MAX_PERIODS) {
              Period np;
              np.day = d;
              np.hour = hour;
              np.minute = minute;
              np.second = second;
              np.delaySeconds = delay;
              examData.periods[examData.numPeriods++] = np;
            }
          }
        }
        saveDataToPreferences();
        String response = "{\"success\":true}";
        server.send(200, "application/json", response);
        return;
      }

      // No explicit range — attempt to detect contiguous group around the index and update them all
      Period base = examData.periods[index];
      bool present[7] = {false,false,false,false,false,false,false};
      for (int i = 0; i < examData.numPeriods; i++) {
        Period &p = examData.periods[i];
        if (p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
          if (p.day >=0 && p.day <=6) present[p.day] = true;
        }
      }
      int start = base.day, end = base.day;
      while (start > 0 && present[start-1]) start--;
      while (end < 6 && present[end+1]) end++;
      if (end - start >= 1) {
        // update all within range
        for (int i = 0; i < examData.numPeriods; i++) {
          Period &p = examData.periods[i];
          if (p.day >= start && p.day <= end && p.hour == base.hour && p.minute == base.minute && p.second == base.second && p.delaySeconds == base.delaySeconds) {
            p.hour = hour;
            p.minute = minute;
            p.second = second;
            p.delaySeconds = delay;
          }
        }
        saveDataToPreferences();
        String response = "{\"success\":true}";
        server.send(200, "application/json", response);
        return;
      }

      // single entry update
      examData.periods[index].day = nd;
      examData.periods[index].hour = hour;
      examData.periods[index].minute = minute;
      examData.periods[index].second = second;
      examData.periods[index].delaySeconds = delay;
      saveDataToPreferences();
      String response = "{\"success\":true}";
      server.send(200, "application/json", response);
    } else {
      String response = "{\"success\":false,\"message\":\"Invalid index\"}";
      server.send(400, "application/json", response);
    }
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleStartExamTimer() {
  examData.timerActive = true;
  
  // Stop normal timer if active
  if (timerData.timerActive) {
    timerData.timerActive = false;
  }
  
  saveDataToPreferences();
  
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleStopExamTimer() {
  examData.timerActive = false;
  
  // Force relay off immediately
  digitalWrite(TIMER_OUTPUT_PIN, LOW);
  relayState.isActive = false;
  
  saveDataToPreferences();
  
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// ==================== PREFERENCES FUNCTIONS ====================
void saveDataToPreferences() {
  StoredTimerData storedTimer = {};
  StoredExamData storedExam = {};
  StoredHolidayData storedHoliday = {};

  storedTimer.numPeriods = timerData.numPeriods;
  storedTimer.timerActive = timerData.timerActive ? 1 : 0;
  for (int i = 0; i < timerData.numPeriods && i < MAX_PERIODS; i++) {
    storedTimer.periods[i] = timerData.periods[i];
  }

  storedExam.numPeriods = examData.numPeriods;
  storedExam.timerActive = examData.timerActive ? 1 : 0;
  for (int i = 0; i < examData.numPeriods && i < EXAM_MAX_PERIODS; i++) {
    storedExam.periods[i] = examData.periods[i];
  }

  storedHoliday.numDates = holidayData.numDates;
  for (int i = 0; i < holidayData.numDates && i < HOLIDAY_MAX_DATES; i++) {
    storedHoliday.dates[i] = holidayData.dates[i];
  }

  prefs.putUChar(PREFS_MAGIC_KEY, PREFS_MAGIC_VALUE);
  prefs.putBytes(PREFS_TIMER_KEY, &storedTimer, sizeof(storedTimer));
  prefs.putBytes(PREFS_EXAM_KEY, &storedExam, sizeof(storedExam));
  prefs.putBytes(PREFS_HOLIDAY_KEY, &storedHoliday, sizeof(storedHoliday));
  prefs.putUChar(PREFS_EXAMEN_KEY, examModeEnabled ? 1 : 0);
  ////Serial.print.println("Data saved to Preferences");
}

void loadDataFromPreferences() {
  if (prefs.getUChar(PREFS_MAGIC_KEY, 0) != PREFS_MAGIC_VALUE) {
    if (migrateFromEepromIfPresent()) {
      return;
    }
    timerData = {};
    examData = {};
    holidayData = {};
    examModeEnabled = false;
    saveDataToPreferences();
    return;
  }

  bool changed = false;
  timerData = {};
  examData = {};
  holidayData = {};
  examModeEnabled = prefs.getUChar(PREFS_EXAMEN_KEY, 0) == 1;

  if (!prefs.isKey(PREFS_EXAMEN_KEY)) changed = true;

  StoredTimerData storedTimer = {};
  StoredExamData storedExam = {};
  StoredHolidayData storedHoliday = {};

  if (prefs.getBytesLength(PREFS_TIMER_KEY) == sizeof(storedTimer)) {
    prefs.getBytes(PREFS_TIMER_KEY, &storedTimer, sizeof(storedTimer));
  } else {
    changed = true;
  }

  if (prefs.getBytesLength(PREFS_EXAM_KEY) == sizeof(storedExam)) {
    prefs.getBytes(PREFS_EXAM_KEY, &storedExam, sizeof(storedExam));
  } else {
    changed = true;
  }

  if (prefs.getBytesLength(PREFS_HOLIDAY_KEY) == sizeof(storedHoliday)) {
    prefs.getBytes(PREFS_HOLIDAY_KEY, &storedHoliday, sizeof(storedHoliday));
  } else {
    changed = true;
  }

  // Load normal timer data (sanitized + de-duplicated)
  uint8_t rawCount = storedTimer.numPeriods;
  timerData.timerActive = storedTimer.timerActive == 1;
  if (rawCount > MAX_PERIODS) rawCount = MAX_PERIODS;

  int validCount = 0;
  for (int i = 0; i < rawCount; i++) {
    Period p = storedTimer.periods[i];
    if (!isValidStoredPeriod(p)) {
      changed = true;
      continue;
    }
    if (hasDuplicateInTimerData(p, validCount)) {
      changed = true;
      continue;
    }
    timerData.periods[validCount++] = p;
  }
  timerData.numPeriods = (uint8_t)validCount;
  if (validCount != rawCount) changed = true;

  // Load exam mode data
  uint8_t rawExamCount = storedExam.numPeriods;
  examData.timerActive = storedExam.timerActive == 1;
  if (rawExamCount > EXAM_MAX_PERIODS) rawExamCount = EXAM_MAX_PERIODS;

  int validExamCount = 0;
  for (int i = 0; i < rawExamCount; i++) {
    Period p = storedExam.periods[i];
    if (!isValidStoredPeriod(p)) {
      changed = true;
      continue;
    }
    bool dup = false;
    for (int j = 0; j < validExamCount; j++) {
      if (periodEquals(examData.periods[j], p)) { dup = true; break; }
    }
    if (dup) {
      changed = true;
      continue;
    }
    examData.periods[validExamCount++] = p;
  }
  examData.numPeriods = (uint8_t)validExamCount;
  if (validExamCount != rawExamCount) changed = true;

  // Load holiday list
  uint8_t rawHolidayCount = storedHoliday.numDates;
  if (rawHolidayCount > HOLIDAY_MAX_DATES) rawHolidayCount = HOLIDAY_MAX_DATES;

  int validHolidayCount = 0;
  for (int i = 0; i < rawHolidayCount; i++) {
    HolidayDate h = storedHoliday.dates[i];
    if (h.year >= 2000 && h.year <= 2099 && h.month >= 1 && h.month <= 12 && h.day >= 1 &&
        h.day <= daysInMonth(h.year, h.month)) {
      holidayData.dates[validHolidayCount++] = h;
    } else {
      changed = true;
    }
  }
  holidayData.numDates = validHolidayCount;
  if (validHolidayCount != rawHolidayCount) changed = true;

  if (changed) {
    saveDataToPreferences();
  }
}

bool migrateFromEepromIfPresent() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    return false;
  }

  bool migrated = false;
  timerData = {};
  examData = {};
  holidayData = {};
  examModeEnabled = false;

  if (EEPROM.read(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC_VALUE) {
    // Migrate from the newer EEPROM layout (4096 bytes).
    uint8_t rawCount = EEPROM.read(TIMER_DATA_OFFSET);
    timerData.timerActive = EEPROM.read(TIMER_DATA_OFFSET + 1) == 1;
    if (rawCount > MAX_PERIODS) rawCount = MAX_PERIODS;

    int validCount = 0;
    for (int i = 0; i < rawCount; i++) {
      int addr = TIMER_PERIODS_OFFSET + i * PERIOD_EEPROM_BYTES;
      Period p;
      p.day = EEPROM.read(addr);
      p.hour = EEPROM.read(addr + 1);
      p.minute = EEPROM.read(addr + 2);
      p.second = EEPROM.read(addr + 3);
      p.delaySeconds = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);

      if (!isValidStoredPeriod(p)) continue;
      if (hasDuplicateInTimerData(p, validCount)) continue;
      timerData.periods[validCount++] = p;
    }
    timerData.numPeriods = (uint8_t)validCount;

    examModeEnabled = EEPROM.read(EXAM_DATA_OFFSET) == 1;
    uint8_t rawExamCount = EEPROM.read(EXAM_DATA_OFFSET + 1);
    examData.timerActive = EEPROM.read(EXAM_DATA_OFFSET + 2) == 1;
    if (rawExamCount > EXAM_MAX_PERIODS) rawExamCount = EXAM_MAX_PERIODS;

    int validExamCount = 0;
    for (int i = 0; i < rawExamCount; i++) {
      int addr = EXAM_PERIODS_OFFSET + i * PERIOD_EEPROM_BYTES;
      Period p;
      p.day = EEPROM.read(addr);
      p.hour = EEPROM.read(addr + 1);
      p.minute = EEPROM.read(addr + 2);
      p.second = EEPROM.read(addr + 3);
      p.delaySeconds = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);

      if (!isValidStoredPeriod(p)) continue;
      bool dup = false;
      for (int j = 0; j < validExamCount; j++) {
        if (periodEquals(examData.periods[j], p)) { dup = true; break; }
      }
      if (dup) continue;
      examData.periods[validExamCount++] = p;
    }
    examData.numPeriods = (uint8_t)validExamCount;

    uint8_t rawHolidayCount = EEPROM.read(HOLIDAY_DATA_OFFSET);
    if (rawHolidayCount > HOLIDAY_MAX_DATES) rawHolidayCount = HOLIDAY_MAX_DATES;
    int validHolidayCount = 0;
    for (int i = 0; i < rawHolidayCount; i++) {
      int addr = HOLIDAY_DATA_OFFSET + 1 + i * HOLIDAY_ENTRY_BYTES;
      uint16_t year = ((uint16_t)EEPROM.read(addr) << 8) | EEPROM.read(addr + 1);
      uint8_t month = EEPROM.read(addr + 2);
      uint8_t day = EEPROM.read(addr + 3);
      if (year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month)) {
        holidayData.dates[validHolidayCount].year = year;
        holidayData.dates[validHolidayCount].month = month;
        holidayData.dates[validHolidayCount].day = day;
        validHolidayCount++;
      }
    }
    holidayData.numDates = validHolidayCount;
    migrated = true;
  } else if (EEPROM.read(EEPROM_OLD_MAGIC_ADDR) == EEPROM_OLD_MAGIC_VALUE) {
    // Best-effort migration from old, overlapping layout.
    timerData.timerActive = EEPROM.read(1) == 1;
    examModeEnabled = EEPROM.read(EEPROM_OLD_EXAM_DATA_OFFSET) == 1;
    examData.timerActive = EEPROM.read(EEPROM_OLD_EXAM_DATA_OFFSET + 2) == 1;

    uint8_t oldTimerCount = EEPROM.read(0);
    if (oldTimerCount > MAX_PERIODS) oldTimerCount = MAX_PERIODS;

    // Old layout overlapped exam data starting at 400. Never read past the safe region.
    const int oldMaxSafeTimer = (EEPROM_OLD_EXAM_DATA_OFFSET - 2) / PERIOD_EEPROM_BYTES; // 66
    if ((int)oldTimerCount > oldMaxSafeTimer) oldTimerCount = (uint8_t)oldMaxSafeTimer;

    for (int i = 0; i < oldTimerCount; i++) {
      int addr = 2 + i * PERIOD_EEPROM_BYTES;
      Period p;
      p.day = EEPROM.read(addr);
      p.hour = EEPROM.read(addr + 1);
      p.minute = EEPROM.read(addr + 2);
      p.second = EEPROM.read(addr + 3);
      p.delaySeconds = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);

      if (!isValidStoredPeriod(p)) continue;
      if (hasDuplicateInTimerData(p, timerData.numPeriods)) continue;
      timerData.periods[timerData.numPeriods++] = p;
    }

    uint8_t oldExamCount = EEPROM.read(EEPROM_OLD_EXAM_DATA_OFFSET + 1);
    if (oldExamCount > EXAM_MAX_PERIODS) oldExamCount = EXAM_MAX_PERIODS;
    for (int i = 0; i < oldExamCount; i++) {
      int addr = EEPROM_OLD_EXAM_DATA_OFFSET + 3 + i * PERIOD_EEPROM_BYTES;
      Period p;
      p.day = EEPROM.read(addr);
      p.hour = EEPROM.read(addr + 1);
      p.minute = EEPROM.read(addr + 2);
      p.second = EEPROM.read(addr + 3);
      p.delaySeconds = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);

      if (!isValidStoredPeriod(p)) continue;
      bool dup = false;
      for (int j = 0; j < examData.numPeriods; j++) {
        if (periodEquals(examData.periods[j], p)) { dup = true; break; }
      }
      if (dup) continue;
      if (examData.numPeriods < EXAM_MAX_PERIODS) {
        examData.periods[examData.numPeriods++] = p;
      }
    }

    uint8_t oldHolidayCount = EEPROM.read(EEPROM_OLD_HOLIDAY_DATA_OFFSET);
    if (oldHolidayCount > HOLIDAY_MAX_DATES) oldHolidayCount = HOLIDAY_MAX_DATES;
    for (int i = 0; i < oldHolidayCount; i++) {
      int addr = EEPROM_OLD_HOLIDAY_DATA_OFFSET + 1 + i * HOLIDAY_ENTRY_BYTES;
      uint16_t year = ((uint16_t)EEPROM.read(addr) << 8) | EEPROM.read(addr + 1);
      uint8_t month = EEPROM.read(addr + 2);
      uint8_t day = EEPROM.read(addr + 3);

      if (!(year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month))) {
        continue;
      }
      bool exists = false;
      for (int j = 0; j < holidayData.numDates; j++) {
        const HolidayDate &h = holidayData.dates[j];
        if (h.year == year && h.month == month && h.day == day) { exists = true; break; }
      }
      if (exists) continue;
      if (holidayData.numDates < HOLIDAY_MAX_DATES) {
        holidayData.dates[holidayData.numDates].year = year;
        holidayData.dates[holidayData.numDates].month = month;
        holidayData.dates[holidayData.numDates].day = day;
        holidayData.numDates++;
      }
    }
    migrated = true;
  }

  if (migrated) {
    saveDataToPreferences();
  }
  EEPROM.end();
  return migrated;
}
