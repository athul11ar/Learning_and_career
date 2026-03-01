# ESP32 Smart Timer System - Complete Documentation

## Project Overview

This is a complete ESP32-based smart timer system designed for automated time-based scheduling. It includes:
- **Real-time clock** (DS3231 RTC module)
- **OLED display** (SSD1306 display)
- **WiFi web interface** for remote control
- **Automated scheduling** with customizable delays
- **Non-volatile storage** (EEPROM) for persistent data

### Use Case Example
**Automated Timetable Scheduler**: Set a base time and then define delays to trigger actions at specific intervals (e.g., class bells, shift changes, equipment activation).

---

## Hardware Requirements

### Components Needed:
1. **ESP32 Development Board** (DevKit v1 or compatible)
2. **DS3231 RTC Module** with coin cell battery
3. **SSD1306 128x64 OLED Display** (0.96 inch)
4. **Relay Module** or **Buzzer** (for output at GPIO 26)
5. **Breadboard and jumper wires**
6. **USB cable** for programming

### Pinout Connections:

```
ESP32 Pin          | Device
------------------+----------------------------------
GPIO 21 (SDA)      | DS3231 SDA + SSD1306 SDA
GPIO 22 (SCL)      | DS3231 SCL + SSD1306 SCL
GPIO 26            | Relay/Buzzer Output
5V                 | DS3231 VCC + SSD1306 VCC
GND                | Ground (common to all modules)
```

### I2C Address Guide:
- **DS3231 RTC**: 0x68
- **SSD1306 Display**: 0x3C

---

## Software Setup

### Prerequisites:
1. **Arduino IDE** (v1.8.x or higher)
2. **ESP32 Board Support** installed in Arduino IDE
3. **Required Libraries** (see next section)

### Step 1: Install Arduino IDE & ESP32 Support

1. Download [Arduino IDE](https://www.arduino.cc/en/software)
2. Open Arduino IDE
3. Go to **File → Preferences**
4. Add this URL in "Additional Boards Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
5. Go to **Tools → Board Manager**
6. Search for "esp32" and install **ESP32 by Espressif Systems**

### Step 2: Install Required Libraries

Open **Sketch → Include Library → Manage Libraries** and search for:

1. **RTClib** by Adafruit
   - Search: "RTClib"
   - Author: Adafruit
   - Install latest version

2. **Adafruit SSD1306** by Adafruit
   - Search: "SSD1306"
   - Author: Adafruit
   - Install latest version

3. **Adafruit GFX Library** by Adafruit (dependency for SSD1306)
   - Search: "Adafruit GFX"
   - Install latest version

### Step 3: Configure Arduino IDE for ESP32

1. **Tools → Board → ESP32 Dev Module**
2. **Tools → Upload Speed → 921600**
3. **Tools → CPU Frequency → 240 MHz (WiFi/BT)**
4. **Tools → Core Debug Level → None**
5. **Tools → PSRAM → Enabled**

### Step 4: Upload Code

1. Copy `smart_timer.ino` into Arduino IDE
2. Select the correct COM port (**Tools → Port**)
3. Click **Upload** (⬆️ button)
4. Wait for "Upload complete" message

---

## Using the System

### Initial Setup:

1. **Power on the ESP32**
   - The display will show initialization messages
   - WiFi AP will start broadcasting: **SSID: SHIBU_TIMER**
   - Password: **timer12345**

2. **Connect to the WiFi**
   - On your laptop/mobile: Find and connect to "SHIBU_TIMER" network
   - Enter password: "timer12345"

3. **Access the Web Interface**
   - Open a browser and go to: **http://192.168.4.1**
   - You should see the Smart Timer control panel

### Web Interface Guide:

#### Section 1: Current Time Display
- Shows the real-time from DS3231
- Updates every second
- Use this to verify RTC is working

#### Section 2: Set Current Time
- **Fields**: Hour (0-23), Minute (0-59), Second (0-59)
- **Action**: Click "Set Time on RTC" to synchronize the system time
- **Use Case**: When you first set up the system or after battery failure

#### Section 3: Set Delay Interval
- **Fields**: Hour, Minute, Second for the delay duration
- **Action**: Click "Configure Delay"
- **Logic**: Timer triggers at = (Set Time) + (Delay)
- **Example**:
  - Set Time: 10:00:00
  - Delay: 00:05:00
  - Trigger at: 10:05:00

#### Section 4: Timer Control
- **Start Timer**: Activates the scheduling system
- **Stop Timer**: Deactivates and resets output
- **Status**: Shows if timer is ACTIVE (running) or INACTIVE

### Scheduled Example - Automated Class Timetable:

```
Class Schedule:
├─ 09:00:00 - Class 1 starts → Output triggers for 5 seconds
├─ 09:45:00 - Class 1 ends → Output triggers for 5 seconds
├─ 10:00:00 - Class 2 starts → Output triggers for 5 seconds
└─ 10:45:00 - Class 2 ends → Output triggers for 5 seconds

Setup Steps:
1. Set Current Time: 08:50:00
2. Configure first bell:
   - Set Time: 09:00:00
   - Delay: 00:00:00
   - Start Timer
3. When triggered, manually change Set Time and reconfigure for next event
```

---

## Technical Details

### Data Structure (Stored in EEPROM):
```cpp
struct TimerData {
  uint8_t setHour;      // Current time set hour (0-23)
  uint8_t setMinute;    // Current time set minute (0-59)
  uint8_t setSecond;    // Current time set second (0-59)
  
  uint8_t delayHour;    // Delay hour (0-23)
  uint8_t delayMinute;  // Delay minute (0-59)
  uint8_t delaySecond;  // Delay second (0-59)
  
  bool timerActive;     // Is timer running?
  uint8_t timerStatus;  // 0: idle, 1: running, 2: triggered
};
```

### Communication Protocols:

#### API Endpoints:
```
GET  /                 → Returns HTML UI
GET  /api/time         → Get current RTC time (JSON)
GET  /api/settime      → Set RTC time
GET  /api/timer        → Get all timer data (JSON)
GET  /api/settimer     → Configure timer
GET  /api/starttimer   → Start timer
GET  /api/stoptimer    → Stop timer
```

#### I2C Communication:
- **Speed**: 100 kHz (standard mode)
- **Bus**: Shared between RTC and Display
- **Pull-ups**: Usually built into modules

### GPIO Output:

**Pin 26** (TIMER_OUTPUT_PIN):
- Goes HIGH when timer is triggered
- Stays HIGH for 5 seconds
- Used to control relay, buzzer, or other devices
- Can be modified by changing `#define TIMER_OUTPUT_PIN 26`

---

## Troubleshooting

### Issue: Display shows "ERROR: RTC not found!"
**Solution**:
- Check I2C connections (SDA/SCL to pins 21/22)
- Use I2C Scanner to verify RTC address (should be 0x68)
- Check if RTC module power is connected
- The DS3231 may have a different address if modified

### Issue: Display shows "SSD1306 allocation failed"
**Solution**:
- Check I2C connections to display
- Verify display address with I2C Scanner (should be 0x3C)
- Try increasing I2C speed or check pull-up resistors
- Some displays use 0x3D instead of 0x3C

### Issue: WiFi network not appearing
**Solution**:
- Check Serial Monitor (baud rate 115200)
- Ensure AP mode is enabled in code
- Try moving closer to ESP32
- Some devices don't see 2.4GHz networks (try with a mobile phone)

### Issue: Timer doesn't trigger
**Solution**:
- Verify RTC has correct time (check Serial Monitor)
- Calculate: trigger_time = set_time + delay
- Check if timer is in ACTIVE state
- Verify EEPROM data is saved correctly
- Check GPIO 26 with a multimeter

### Issue: Time keeps resetting
**Solution**:
- DS3231 battery may be dead (not critical, use WiFi to set time each session)
- Replace coin cell battery in RTC module
- Check for loose connections

---

## Serial Monitor Debug Output

If you open **Tools → Serial Monitor** (Baud: 115200), you'll see:

```
=== ESP32 Smart Timer Initialization ===
Display initialized successfully
RTC initialized successfully
AP IP address: 192.168.4.1
Web server started on port 80
RTC time set to: 10:05:30
Timer set - Trigger at: 10:05:30 + 00:05:00
Timer started
TIMER TRIGGERED!
```

This helps verify system status during operation.

---

## Advanced Configuration

### Modify WiFi Credentials:

Edit in `smart_timer.ino`:
```cpp
const char* ssid = "MY_NETWORK";
const char* password = "mypassword123";
```

### Modify Output Pin:

```cpp
#define TIMER_OUTPUT_PIN 27  // Change 26 to desired pin
```

### Modify Output Duration:

In `triggerTimer()` function:
```cpp
delay(10000);  // Change from 5000 (5 sec) to 10000 (10 sec)
```

### Modify I2C Pins:

```cpp
#define SDA_PIN 21  // Change to desired pin
#define SCL_PIN 22  // Change to desired pin
```

---

## Power Specifications

- **ESP32 USB Power**: 5V, 500mA minimum
- **Current Draw**: ~150mA (WiFi active)
- **Modules**: DS3231 + SSD1306 draw <50mA
- **Relay Output**: Can sink/source up to 25mA (use transistor for larger loads)

---

## Multi-Timer Extension (Advanced)

To support multiple scheduled events, modify the `TimerData` structure to an array:

```cpp
#define MAX_SCHEDULES 5

struct Schedule {
  uint8_t hour, minute, second;
  uint8_t delayHour, delayMinute, delaySecond;
  bool active;
};

Schedule schedules[MAX_SCHEDULES];
```

Then loop through all schedules in `checkAndExecuteTimer()`.

---

## Maintenance

- **Weekly**: Verify time accuracy (resync if drifting >1 min/week)
- **Monthly**: Check battery backup in RTC (if using)
- **Yearly**: Replace coin cell battery in DS3231 if used in battery-backup mode

---

## Safety Features

- EEPROM auto-save prevents data loss
- RTC has independent power supply
- Web interface has no authentication (use with trusted networks)
- Output triggers automatically reset after 5 seconds

---

## Support & Resources

- **ESP32 Documentation**: https://docs.espressif.com/projects/esp-idf/
- **Adafruit RTClib**: https://github.com/adafruit/RTClib
- **Arduino WebServer**: https://github.com/espressif/arduino-esp32/tree/master/libraries/WebServer
- **DS3231 Datasheet**: https://datasheets.maximintegrated.com/en/ds/DS3231.pdf

---

## License

This project is open for educational and personal use.

---

**Version**: 1.0  
**Last Updated**: February 2026  
**Author**: Smart Timer Project Team
