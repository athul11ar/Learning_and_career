# Wiring Diagram & Pinout Guide

## ESP32 Smart Timer - Hardware Connections

This document provides detailed wiring instructions for the ESP32 Smart Timer project.

---

## Quick Pinout Reference

```
ESP32 PIN          | CONNECTED TO              | PURPOSE                  | I2C BUS
=====================================|================================================
GPIO 21 (SDA)      | DS3231 SDA                | I2C Bus 0 Data Line      | I2C_0
GPIO 22 (SCL)      | DS3231 SCL                | I2C Bus 0 Clock Line     | I2C_0
GPIO 4 (SDA)       | SSD1306 SDA               | I2C Bus 1 Data Line      | I2C_1
GPIO 5 (SCL)       | SSD1306 SCL               | I2C Bus 1 Clock Line     | I2C_1
GPIO 26            | Relay/Buzzer Module       | Timer Output Trigger     | GPIO
5V (USB)           | VCC on RTC & Display      | Power Supply             | -
GND                | GND on all modules        | Ground (Common)          | -
```

---

## Complete Wiring Diagram

```
                    ┌──────────────────────┐
                    │     ESP32 Dev        │
                    │     Module           │
                    └──────────────────────┘
               
        I2C Bus 0                   I2C Bus 1
        (RTC)                       (Display)
        │   │                       │   │
    GPIO21 GPIO22                GPIO4 GPIO5
    (SDA) (SCL)                  (SDA) (SCL)
        │   │                       │   │
        │   │                       │   │
    [DS3231 RTC]              [SSD1306 OLED]
    I2C: 0x68                I2C: 0x3C
        │                       │
        └───────GND─────────┬───GND
                 │          │
                5V──VCC─────+
```

**Key Improvement**: OLED display now uses separate I2C bus (GPIO 4/5) to eliminate conflicts with RTC!

---

## Step-by-Step Connections

### 1. ESP32 to DS3231 RTC Module

| ESP32 Pin | DS3231 Pin | Wire Color | Description |
|-----------|------------|------------|-------------|
| GPIO 21   | SDA        | Yellow     | I2C Data (Serial Data) |
| GPIO 22   | SCL        | Green      | I2C Clock (Serial Clock) |
| 5V        | VCC        | Red        | Power Supply (+5V) |
| GND       | GND        | Black      | Ground |

**DS3231 Module Footer** (viewed from underside):
```
┌─────────────────────┐
│ GND │ VCC │ SDA │ SCL │
└─────────────────────┘
  │     │     │     │
  B     R     Y     G
```

---

### 2. ESP32 to SSD1306 Display Module (I2C Bus 1)

| ESP32 Pin | SSD1306 Pin | Wire Color | Description |
|-----------|-------------|------------|-------------|
| GPIO 4    | SDA         | Yellow     | I2C Bus 1 Data (Serial Data) |
| GPIO 5    | SCL         | Green      | I2C Bus 1 Clock (Serial Clock) |
| 5V        | VCC         | Red        | Power Supply (+5V) |
| GND       | GND         | Black      | Ground |

**SSD1306 Module Footer** (viewed from underside):
```
┌──────────────────────────┐
│ GND │ VCC │ SCL │ SDA │
└──────────────────────────┘
  │     │     │     │
  B     R     G     Y
```

**Note**: SSD1306 now uses GPIO 4 (SDA) and GPIO 5 (SCL) instead of GPIO 21/22 to avoid I2C conflicts with the RTC module.

---

### 3. ESP32 to Relay/Output Module (GPIO 26)

#### Option A: Relay Module (For High Power Devices)

```
ESP32 GPIO 26 ──────→ IN+ (Signal) on Relay Module
ESP32 GND     ──────→ GND on Relay Module
Relay Module  ──────→ Controls High Power Device
```

**Relay Module Pinout**:
```
Front View:
┌──────────────────┐
│ IN+ │ IN- │ GND  │  (Input Side - Connect to ESP32)
├──────────────────┤
│ NO  │ COM │ NC   │  (Output Side - For external device)
└──────────────────┘

NO  = Normally Open (default: disconnected)
COM = Common (wire to device)
NC  = Normally Closed (default: connected)
GND = Ground reference
```

#### Option B: Buzzer Module (Direct Connection)

```
ESP32 GPIO 26 ──────→ Positive (+) on Buzzer
ESP32 GND     ──────→ Negative (-) on Buzzer
```

#### Option C: LED Module (Indicator)

```
ESP32 GPIO 26 ──────→ Anode (+) of LED [with 330Ω resistor]
Resistor      ──────→ Cathode (-) of LED
ESP32 GND     ──────→ Other side of Resistor
```

---

## I2C Bus Configuration

### I2C Addressing

The I2C bus on GPIO 21 (SDA) and GPIO 22 (SCL) can support multiple devices:

```
ESP32 Master (I2C Host)
    └─ GPIO 21-22 (SDA-SCL)
       ├─ DS3231 (Address: 0x68)
       └─ SSD1306 (Address: 0x3C)
```

### Pull-up Resistors

- Most modules have built-in pull-up resistors (4.7kΩ)
- If you experience communication issues, you may need to add external 4.7kΩ pull-ups:

```
        ┌─── 4.7kΩ ─── 5V
        │
    SDA ┤
        │
        └─── To DS3231 + SSD1306

        ┌─── 4.7kΩ ─── 5V
        │
    SCL ┤
        │
        └─── To DS3231 + SSD1306
```

---

## Power Supply Configuration

### USB Power Method (Recommended)
```
USB Cable
    │
    └─── USB-C or Micro-USB port on ESP32
         │
         ├─► 5V (for RTC & Display)
         └─► GND (ground)
```

### Battery Backup (Optional - DS3231 only)

The DS3231 has a CR2032 coin cell slot for power backup:

```
DS3231 Module
    │
    ├─ Coin Cell Slot: Insert CR2032 battery
    │  (Keeps RTC running during power loss)
    │
    └─ Main Power: Still needed from ESP32
```

---

## Soldering Guide (If Not Using Breadboard)

### Pre-Soldering Checklist:
- [ ] Soldering iron ready (350-400°C)
- [ ] Solder (lead-free recommended)
- [ ] Wet sponge for iron cleaning
- [ ] Helping hands or PCB holder
- [ ] Flux pen (optional but helpful)

### Soldering Steps:

1. **Clean the Iron**: Wipe on wet sponge
2. **Apply Flux**: Optional, helps solder flow
3. **Heat Pad & Pin**: Touch iron to both for 1-2 seconds
4. **Apply Solder**: Feed solder to heated joint
5. **Remove Iron**: Take iron away first, then solder will solidify

### Good vs Bad Solder Joints:

```
Good Joint (✓)          Bad Joint (✗)
Shiny appearance        Dull/grainy look
Cone shape              Blob or bridge
Strong connection       Weak connection
Small amount            Excess solder
```

---

## Test Connections Before Power

### Before Connecting Power, Verify:

1. **No Short Circuits**:
   - VCC and GND wires are NOT touching
   - ESP32 pins are correctly identified
   - Modules are not touching each other

2. **Correct Wire Placement**:
   - SDA on same pins (GPIO 21)
   - SCL on same pins (GPIO 22)
   - All GND connected together

3. **Module Health**:
   - Visual inspection for damage
   - Loose components wiggle test
   - No burnt marks

### Power-On Test:
1. Connect USB to ESP32
2. Check if modules light up (RTC LED, Display backlight)
3. Listen for any unusual sounds (hissing, buzzing)
4. Feel if any component gets too hot (they shouldn't)

---

## Troubleshooting Connections

### Issue: Display Not Showing Anything

**Check List**:
1. Is SDA connected to GPIO 21? → Check yellow wire
2. Is SCL connected to GPIO 22? → Check green wire
3. Is VCC getting 5V? → Use multimeter to test
4. Is GND properly connected? → Check black wire continuity
5. Display I2C address 0x3C? → Use I2C Scanner sketch

**I2C Scanner Test Code**:
```cpp
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);  // SDA=21, SCL=22
  Serial.println("I2C Scanner Starting...");
}

void loop() {
  for (byte i = 8; i < 120; i++) {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0) {
      Serial.print("I2C device found at address 0x");
      Serial.println(i, HEX);
    }
  }
  delay(5000);
}
```

### Issue: RTC Losing Time

**Check List**:
1. Is DS3231 powered? → Check 5V connection
2. Battery backup? → Insert CR2032 into coin cell slot
3. Crystal oscillator damaged? → Visual inspection
4. Loose connection? → Gently wiggle wires

### Issue: GPIO 26 Output Not Working

**Check List**:
1. Is relay/buzzer connected? → Double-check wiring
2. Is module powered separately? → Some relays need separate 5V
3. Is GPIO 26 set to OUTPUT? → Code should have `pinMode(26, OUTPUT)`
4. Test with multimeter? → Should show 0V-3.3V

---

## Alternative GPIO Pins

If GPIO 21/22 or GPIO 26 are unavailable, alternatives:

### I2C Alternate Pins:
```
Primary:   SDA=21, SCL=22
Alternate: SDA=25, SCL=26  (but conflicts with output pin)
Alternate: SDA=26, SCL=27  (but 27 may not be available)
```

### Output Alternate Pins:
```
Primary:   GPIO 26
Safe Alt:  GPIO 32 (if available)
Avoid:     GPIO 34-39 (input only)
```

To change pins, modify at top of code:
```cpp
#define SDA_PIN 25
#define SCL_PIN 26
#define TIMER_OUTPUT_PIN 32
```

---

## Cable Length Guidelines

| Component | Max Cable Length | Notes |
|-----------|-----------------|-------|
| I2C (SDA/SCL) | 1 meter | Keep fairly short for reliability |
| Power (5V/GND) | 1.5 meters | Use thicker wire (22AWG) if long |
| GPIO Output | 2 meters | Depends on relay shielding |

**Best Practice**: Keep cables under 30cm if possible, especially I2C lines.

---

## Component Size Reference

For breadboard planning:

```
DS3231 Module:      ~35mm x 20mm x 10mm  (4 pins)
SSD1306 Display:    ~40mm x 27mm x 5mm   (4 pins)
Relay Module:       ~30mm x 30mm x 20mm  (3-6 pins)
Breadboard (half):  ~170mm x 55mm        (Good for all)
```

---

## Connectors Guide (Optional)

Instead of breadboards, you can use connectors:

| Connector Type | Use Case | Advantage |
|----------------|----------|-----------|
| JST-PH 2.0mm | Module power | Keyed, won't reverse |
| DuPont 2.54mm | All connections | Standard, reusable |
| Screw terminal | Relay output | Secure, no soldering |
| XT60 | High power relay | Can handle higher current |

---

## Safety Tips

⚠️ **Always** follow these safety guidelines:

1. **Never** reverse power (5V and GND)
2. **Never** power multiple components simultaneously on first test
3. **Always** use appropriate resistors (especially for GPIO outputs)
4. **Always** check connections before applying power
5. **Always** disconnect before making changes
6. **Never** work on live circuits
7. **Always** use multimeter to verify connections

---

## Diagram Legend

```
─────  Wire/Connection
[XXX]  Component/Device
│ ├ └  Junction/Splitter
(O)    Pin/Connector
─E─    Resistor
```

---

**Last Updated**: February 2026  
**Device Tested**: ESP32 DevKit v1, DS3231, SSD1306 128x64
