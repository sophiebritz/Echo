Wiring Diagram
ESP32 CONNECTIONS:
===================

ROTARY ENCODER (STEMMA QT):
├─ SDA (Blue)    → GPIO 21 (ESP32 SDA)
├─ SCL (Yellow)  → GPIO 22 (ESP32 SCL)
├─ VCC (Red)     → 3.3V
└─ GND (Black)   → GND

OLED DISPLAY (I2C):
├─ SDA → GPIO 21 (same as encoder - shared I2C bus)
├─ SCL → GPIO 22 (same as encoder - shared I2C bus)  
├─ VCC → 3.3V
└─ GND → GND

NEOPIXEL RING:
├─ DIN  → GPIO 25
├─ VCC  → 5V (or 3.3V for <10 LEDs)
└─ GND  → GND

POWER:
└─ ESP32 powered via USB (5V)
Breadboard layout:
[ESP32 Board]
              |
    ┌─────────┼─────────┐
    │         │         │
[Encoder] [Display] [LEDs]
   I2C       I2C      GPIO25