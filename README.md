# ESP32 IoT Child Safety Wearable Device

**Academic prototype | Arduino IDE | ESP32 | Embedded systems and IoT**

## Project overview
This project explores a child-safety monitoring prototype built around an ESP32 and programmed in **Arduino IDE**. The firmware combines temperature and pulse monitoring, a local LCD, touch-triggered SOS alerts, a Wi-Fi-hosted monitoring dashboard, and Twilio-based WhatsApp notification requests.

> **Scope:** This repository documents an academic prototype, not a certified safety or medical device. Report screenshots illustrate the project's documented outputs; the source code is provided for study and demonstration.

## Hardware and software

| Component / tool | Purpose |
| --- | --- |
| ESP32 | Main controller, Wi-Fi connection and local HTTP server |
| MAX30100 pulse-oximeter module | Heartbeat detection through I2C (the shared firmware uses MAX30100, not MAX30102) |
| Analog temperature sensor (TMP36-style conversion in code) | Temperature sampling and software calibration |
| Capacitive touch input | SOS trigger |
| 16×2 I2C LCD | Displays temperature, heart rate and SOS messages |
| Buzzer | Audible SOS indication |
| Arduino IDE / Arduino ESP32 core | Firmware development |
| Twilio WhatsApp API | Sends emergency notification requests over Wi-Fi |
| HTML, JavaScript, Leaflet/OpenStreetMap | Local monitoring dashboard with map |

## How the prototype works
1. The ESP32 initializes the LCD, pulse sensor, temperature input and Wi-Fi connection.
2. It periodically reads and calibrates temperature and updates the LCD.
3. It estimates heart rate from detected beat intervals when valid sensor data is available.
4. The ESP32 serves a local web dashboard displaying readings, location and SOS state.
5. A touch input or dashboard button can activate SOS; the device sounds the buzzer and attempts a WhatsApp alert through Twilio.
6. The dashboard may supply browser geolocation to the ESP32. If unavailable, firmware attempts approximate IP-based geolocation; **this firmware does not directly interface with a GPS module**.

## Firmware implementation
- **I2C:** MAX30100 pulse sensing and LCD interface (SDA GPIO 21, SCL GPIO 22).
- **ADC:** Temperature sensor on GPIO 35, sampled with averaging and a stored calibration offset.
- **Digital I/O:** Touch input GPIO 4, buzzer GPIO 25 and LED GPIO 2.
- **Networking:** ESP32 `WebServer` endpoints `/`, `/status`, `/reportLocation` and `/sos`.
- **Notifications:** Twilio REST API request over Wi-Fi; **no SIM800L cellular modem interface appears in this firmware**.

## Demonstration results

The academic project report includes the system architecture, circuit diagrams, implementation details, and documented output images.

**Project report:** [View the complete project report](mini%20project%20report-1%20%281%29.pdf)

The report documents the overall academic prototype. The Arduino firmware in this repository represents the shared implementation and may differ from some hardware configurations described in the report.

## Repository files

- `README.md` — Project description and implementation summary.
- `ChildTracker_Sanitized.ino` — Arduino IDE firmware with sensitive credentials replaced by placeholders.
- `mini project report-1 (1).pdf` — Academic project report containing design documentation and output images.

## Reproducing the demonstration
Open the `.ino` sketch in **Arduino IDE** with ESP32 board support installed. Install the required `MAX30100_PulseOximeter` and `LiquidCrystal_I2C` libraries, and provide your own Wi-Fi and Twilio configuration **privately**. The web dashboard uses Leaflet/OpenStreetMap resources. Hardware wiring, sensor compatibility, Twilio setup and network connectivity must be checked before attempting to run the prototype.

## Limitations and safety notes
- The shared firmware generates **synthetic fallback BPM values** if heart-rate readings are unavailable; these values are **not measured physiological data**.
- The prototype currently contains an **insecure TLS option** for Twilio communications. Replace it with proper certificate validation before any real-world deployment.
- Location from browser geolocation depends on user permission and browser security requirements; IP geolocation is approximate.
- No accuracy validation, reliability testing or certified emergency response performance is claimed.
- Never publish Wi-Fi passwords, Twilio authentication tokens or personal recipient numbers. Rotate credentials that have already been exposed.

## Tools and learning
**ESP32 · Arduino IDE · C/C++ · I2C · ADC · Wi-Fi · HTTP · HTML/JavaScript · Twilio API · IoT monitoring**
