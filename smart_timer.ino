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

// flag used when RTC initialization fails to avoid reset loops
bool rtcAvailable = true;
bool displayAvailable = true;  // track if display initialized successfully
bool displayPoweredOn = true;

// ==================== GLOBAL VARIABLES ====================
#define MAX_PERIODS 210

// EEPROM layout:
// 0: numPeriods
// 1: timerActive
// 2.. : periods (6 bytes each)
// MAGIC at last byte to detect first-run
#define EEPROM_MAGIC_ADDR 511
#define EEPROM_MAGIC_VALUE 0xA5

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

struct RelayState {
  bool isActive = false;
  unsigned long activationTime = 0;
  uint32_t delayDuration = 0;
};

TimerData timerData;
RelayState relayState;
unsigned long lastDisplayUpdate = 0;
unsigned long lastScheduleCheck = 0;

// button state tracking
int lastButtonState = HIGH;
unsigned long buttonPressStart = 0;

// scrolling support for wifi/ssid text on display
int wifiScrollOffset = SCREEN_WIDTH;

// ==================== FORWARD DECLARATIONS ====================
void loadDataFromEEPROM();
void saveDataToEEPROM();
void setupWebServer();
void handleRoot();
void handleGetTime();
void handleSetTime();
void handleGetPeriods();
void handleAddPeriod();
void handleDeletePeriod();
void handleClearPeriods();
void handleEditPeriod();
void handleStartTimer();
void handleStopTimer();
void handleTrigger();
void handleNotFound();
void checkAndExecuteSchedules();
void updateDisplay();
void handleButtonPress();
String getHTMLContent();

// ==================== INITIALIZATION ====================
void setup() {
  Serial.begin(115200);
  delay(500);
  //Serial.print.println("\n\n=== ESP32 Smart Timer Initialization ===");
  
  // Disable watchdog timer to prevent reset issues during relay activation
  // The watchdog would trigger if the main loop is blocked for too long
  disableWatchdog();
  
  // Initialize EEPROM for storing timer data
  EEPROM.begin(512);
  //Serial.print.println("[SETUP] EEPROM initialized");
  
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
  loadDataFromEEPROM();
  
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
          if (displayPoweredOn) {
            display.setPowerSave(1);    // turn display off
            displayPoweredOn = false;
          } else {
            display.setPowerSave(0);    // turn display back on
            displayPoweredOn = true;
          }
        }
      } else if (dur >= 1000) {        // medium press – 2‑second output
        triggerTimer(2);
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
  snprintf(buffer, sizeof(buffer), "%s %d/%d/%04d", dayNames[now.dayOfTheWeek()], now.day(), now.month(), now.year());
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
  snprintf(buffer, sizeof(buffer), "Periods:%d Active:%s", timerData.numPeriods, timerData.timerActive ? "YES" : "NO");
  safeDisplayDrawStr(0, 48, buffer);
  
  // compute next scheduled event
  uint8_t currentDay = now.dayOfTheWeek();
  uint32_t currentSeconds = (now.hour() * 3600) + (now.minute() * 60) + now.second();
  int nextIndex = -1;
  uint32_t minDiff = UINT32_MAX;
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
  if (nextIndex >= 0) {
    snprintf(buffer, sizeof(buffer), "Next: %02d:%02d:%02d", 
             timerData.periods[nextIndex].hour,
             timerData.periods[nextIndex].minute,
             timerData.periods[nextIndex].second);
    safeDisplayDrawStr(0, 60, buffer);
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
void checkAndExecuteTimer() {
  if (!timerData.timerActive) return;
  if (!rtcAvailable) return;
  
  DateTime now = rtc.now();
  uint8_t currentDay = now.dayOfTheWeek(); // 0=Sun, 1=Mon, ..., 6=Sat
  uint32_t currentSeconds = (now.hour() * 3600) + (now.minute() * 60) + now.second();
  
  static uint32_t lastTriggeredSeconds = UINT32_MAX;
  
  // Check if we've already triggered this second
  if (lastTriggeredSeconds == currentSeconds) {
    return; // Already triggered in this second
  }
  
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
  server.on("/api/addperiod", handleAddPeriod);
  server.on("/api/deleteperiod", handleDeletePeriod);
  server.on("/api/clearperiods", handleClearPeriods);
  server.on("/api/editperiod", handleEditPeriod);      // new endpoint for editing
  server.on("/api/starttimer", handleStartTimer);
  server.on("/api/stoptimer", handleStopTimer);
  server.on("/api/trigger", handleTrigger);            // manual trigger API
  server.onNotFound(handleNotFound);
  
  server.begin();
  ////Serial.print.println("Web server started on port 80");
}

void handleRoot() {
  String html = getHTMLContent();
  server.send(200, "text/html", html);
}

String getHTMLContent() {
  return R"rawliteral(
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
            color: #666;
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
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>⏱ Smart School Timer</h1>
        <p class="subtitle">Control your automated scheduler</p>
        
        <!-- Current Time Display -->
        <div class="section">
            <div class="section-title">Current Date & Time on Timer</div>
            <div class="status-display">
                <div class="status-label">Current Date & Time</div>
                <div class="status-value" id="currentTime" style="font-size: 16px;">--/--/-- --:--:--</div>
                <div class="status-label" style="margin-top: 8px;">Day of Week</div>
                <div class="status-value" id="currentDayOfWeek">---</div>
            </div>
        </div>
        
        <!-- Set Time Section -->
        <div class="section">
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
        <div class="section">
            <div class="section-title">Add Scheduled Period</div>
            <!-- mode selector: which days should receive this period -->
            <div style="margin-bottom: 15px; text-align: center;">
                <label for="periodMode" style="font-size:12px; color:#666; margin-right:5px;">Apply to:</label>
                <select id="periodMode" class="time-input" style="width:150px; margin-right:10px;">
                    <option value="single" selected>Single day</option>
                    <option value="weekdays">Mon‑Fri</option>
                    <option value="saturday">Saturday only</option>
                    <option value="everyday">All days</option>
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
                <input type="number" id="periodDelay" class="time-input" min="0" max="60" placeholder="60" style="width: 100px;">
                <span style="font-size: 12px; color: #666; margin-left: 5px;">seconds</span>
            </div>
            <button class="btn-primary" onclick="addPeriod()">Add Period</button>
        </div>
        
        <!-- Periods List Section -->
        <div class="section">
            <div class="section-title">Scheduled Periods</div>
            <div id="periodsList" class="status-display" style="text-align: left; max-height: 200px; overflow-y: auto;">
                <div class="status-label">No periods added yet.</div>
            </div>
            <div style="margin-top:8px; text-align: right;">
              <button class="btn-danger" style="padding:8px 12px; font-size:12px;" onclick="clearAllPeriods()">Clear All Periods</button>
            </div>
        </div>
        
        <!-- Manual Trigger Section for real-time testing -->
        <div class="section">
            <div class="section-title">Manual Trigger (Emergency Bell)</div>
            <div style="margin-bottom: 15px; text-align: center;">
                <span style="font-size: 12px; color: #666; margin-right: 10px;">Delay:</span>
                <input type="number" id="manualDelay" class="time-input" min="0" max="60" placeholder="60" style="width: 100px;">
                <span style="font-size: 12px; color: #666; margin-left: 5px;">seconds</span>
            </div>
            <button class="btn-secondary" onclick="triggerNow()">Trigger Now</button>
        </div>
        
        <!-- Timer Status Section -->
        <div class="section">
            <div class="section-title">Timer Control (ON/OFF)</div>
            <div id="timerStatus" class="timer-status status-inactive">
                ⭕ Timer is INACTIVE
            </div>
            <div class="button-group">
                <button class="btn-primary" onclick="startTimer()">Timer ON</button>
                <button class="btn-danger" onclick="stopTimer()">Timer OFF</button>
            </div>
        </div>
        
        <!-- Status Messages -->
        <div class="error" id="errorMessage"></div>
        <div class="success" id="successMessage"></div>
    </div>
    
    <script>
        const dayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
        const fullDayNames = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
        
        // helper: automatically move focus when field length reached
        function setupAutoAdvance(srcId, maxLen, nextId) {
            const el = document.getElementById(srcId);
            if (!el) return;
            el.addEventListener('input', () => {
                if (el.value.length >= maxLen && nextId) {
                    const nxt = document.getElementById(nextId);
                    if (nxt) nxt.focus();
                }
            });
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
            let day = parseInt(document.getElementById('periodDay').value);
            if (isNaN(day) || day < 1 || day > 7) {
                showError('Invalid day selected');
                return;
            }
            url += '&day=' + (day);
            } else {
                url += '&group=' + mode; // server will replicate days
            }

            fetch(url)
              .then(response => response.json())
              .then(data => {
                if (data.success) {
                  showSuccess('✓ Period(s) added successfully!');
                  // Clear inputs
                  document.getElementById('periodHour').value = '';
                  document.getElementById('periodMinute').value = '';
                  document.getElementById('periodSecond').value = '';
                  document.getElementById('periodDelay').value = '0';
                  document.getElementById('periodMode').value = 'single';
                  document.getElementById('periodDay').disabled = false;
                  loadPeriods(); // Refresh the list
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
                });
        }
        
        // Function to detect group type based on days pattern
        function detectGroupType(days) {
            days.sort((a, b) => a - b);
            const dayStr = days.join(',');
            
            // Check for known patterns
            if (dayStr === '1,2,3,4,5') return 'Mon-Friday';
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
                groups[key].indices.push(idx);
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
            
            // make sure the day dropdown state matches the default mode
            document.getElementById('periodMode').dispatchEvent(new Event('change'));            

            // Update time every second
            setInterval(updateCurrentTime, 1000);
            
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

    if (timerData.numPeriods + dayCount > MAX_PERIODS) {
      String response = "{\"success\":false,\"message\":\"Not enough space for all selected days (limit " + String(MAX_PERIODS) + ")\"}";
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
      timerData.periods[timerData.numPeriods++] = newPeriod;
    }

    saveDataToEEPROM();

    String response = "{\"success\":true}";
    server.send(200, "application/json", response);
  } else {
    String response = "{\"success\":false,\"message\":\"Missing parameters\"}";
    server.send(400, "application/json", response);
  }
}

void handleDeletePeriod() {
  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < timerData.numPeriods) {
      for (int i = index; i < timerData.numPeriods - 1; i++) {
        timerData.periods[i] = timerData.periods[i + 1];
      }
      timerData.numPeriods--;
      saveDataToEEPROM();
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

// clear all scheduled periods
void handleClearPeriods() {
  // reset in-memory
  timerData.numPeriods = 0;
  timerData.timerActive = false;

  // clear EEPROM area used for periods to avoid stale data
  for (int i = 0; i < MAX_PERIODS; i++) {
    int addr = 2 + i * 6;
    EEPROM.write(addr, 0);
    EEPROM.write(addr + 1, 0);
    EEPROM.write(addr + 2, 0);
    EEPROM.write(addr + 3, 0);
    EEPROM.write(addr + 4, 0);
    EEPROM.write(addr + 5, 0);
  }

  // save header and magic
  saveDataToEEPROM();

  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
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
      saveDataToEEPROM();
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
  saveDataToEEPROM();
  
  ////Serial.print.println("Timer started");
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleStopTimer() {
  timerData.timerActive = false;
  
  // Force relay off immediately
  digitalWrite(TIMER_OUTPUT_PIN, LOW);
  relayState.isActive = false;
  
  saveDataToEEPROM();
  
  ////Serial.print.println("Timer stopped");
  String response = "{\"success\":true}";
  server.send(200, "application/json", response);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// ==================== EEPROM FUNCTIONS ====================
void saveDataToEEPROM() {
  EEPROM.write(0, timerData.numPeriods);
  EEPROM.write(1, timerData.timerActive ? 1 : 0);

  for (int i = 0; i < timerData.numPeriods; i++) {
    int addr = 2 + i * 6;
    EEPROM.write(addr, timerData.periods[i].day);
    EEPROM.write(addr + 1, timerData.periods[i].hour);
    EEPROM.write(addr + 2, timerData.periods[i].minute);
    EEPROM.write(addr + 3, timerData.periods[i].second);
    EEPROM.write(addr + 4, timerData.periods[i].delaySeconds >> 8); // high byte
    EEPROM.write(addr + 5, timerData.periods[i].delaySeconds & 0xFF); // low byte
  }

  // mark EEPROM as initialized
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VALUE);
  EEPROM.commit();
  ////Serial.print.println("Data saved to EEPROM");
}

void loadDataFromEEPROM() {
  // Check magic byte to determine if EEPROM was initialized by this firmware
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC_VALUE) {
    // First run — clear saved data (leave periods empty)
    timerData.numPeriods = 0;
    timerData.timerActive = false;
    // write default back so next boot reads valid data
    EEPROM.write(0, timerData.numPeriods);
    EEPROM.write(1, timerData.timerActive ? 1 : 0);
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VALUE);
    EEPROM.commit();
    return;
  }

  timerData.numPeriods = EEPROM.read(0);
  timerData.timerActive = EEPROM.read(1) == 1;

  if (timerData.numPeriods > MAX_PERIODS) timerData.numPeriods = MAX_PERIODS;

  for (int i = 0; i < timerData.numPeriods; i++) {
    int addr = 2 + i * 6;
    timerData.periods[i].day = EEPROM.read(addr);
    timerData.periods[i].hour = EEPROM.read(addr + 1);
    timerData.periods[i].minute = EEPROM.read(addr + 2);
    timerData.periods[i].second = EEPROM.read(addr + 3);
    timerData.periods[i].delaySeconds = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);
  }

  ////Serial.print.println("Data loaded from EEPROM");
}
