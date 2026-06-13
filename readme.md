# Medicine Reminder & Compliance System

This project is an ESP32-based medicine reminder device designed to help users take their medication on time and provide basic compliance feedback. It combines a schedule-based reminder system with visual, audible, and sensor-based confirmation to improve reliability.

## Overview

The system runs on an ESP32 and displays information on an OLED screen. It can:
- remind the user when a scheduled dose is due,
- warn about expired medication,
- warn about unsafe temperature or humidity conditions,
- confirm medication intake using simple sensors.

## Main Features

- Scheduled medicine reminders
  - Each medicine can have one or more reminder times.
  - The device triggers an alert when the scheduled time is reached.

- Visual and audible alerts
  - The OLED display shows active reminders and status information.
  - A buzzer sounds during active alerts.

- Expiry alerts
  - Medicines with passed expiry dates trigger a high-priority alert.
  - Expiry alerts can be acknowledged by pressing the button.

- Environmental warnings
  - The device reads temperature and humidity using a DHT11 sensor.
  - If storage conditions become unsafe, an environmental alert is raised.

- Simple user interaction
  - A button can dismiss or acknowledge alerts.
  - Double-pressing the button shows the full schedule view.

## How Ingestion Is Confirmed

The device confirms medication intake using a combination of two sensors:

1. IR sensor
   - Detects a hand or object passing near the device.
   - This is treated as an IR edge event.

2. Water sensor
   - Detects a sudden drop in the sensor reading, which simulates a glass or cup being picked up or moved.

Ingestion is considered confirmed only when both events happen together:
- the IR sensor detects motion or presence, and
- the water sensor detects a drop in value.

This prevents accidental confirmation from a single sensor trigger and makes the confirmation more meaningful.

### Active Alert Confirmation

If a medicine reminder is currently active:
- the user must perform the IR + water confirmation gesture,
- the system then logs the dose as taken for that scheduled slot,
- the reminder is cleared.

### Passive Confirmation

If no alert is active, the same gesture can still be used to log a dose passively within a small time window around a pending schedule slot. This helps capture medication intake even when the user did not respond to the alert directly.

## Hardware

The system uses:
- ESP32 microcontroller
- SSD1306 OLED display
- DHT11 temperature and humidity sensor
- IR sensor
- Water level sensor
- Buzzer
- Push button

## Software / Libraries

The sketch uses the following Arduino libraries:
- Adafruit_GFX
- Adafruit_SSD1306
- DHT sensor library

## Notes

- The clock is currently simulated for demonstration purposes.
- Timing thresholds such as the water drop sensitivity and passive confirmation window can be adjusted in the sketch.
- This project is intended as a prototype and demonstration of reminder and compliance logic.

## How to Use

1. Upload the sketch to an ESP32 board using the Arduino IDE.
2. Connect the sensors and display according to the pin definitions in the sketch.
3. Power the device and observe the reminder and confirmation behavior.
4. Use the button to acknowledge alerts or view the full schedule.
