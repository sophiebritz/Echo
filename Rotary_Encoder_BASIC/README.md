# 🚣 Rowing Machine Monitor for Elderly Fitness

ESP32-based tracker using rotary encoder attached to rowing machine cable wheel.

## Hardware Required
- ESP32 Dev Board
- Adafruit I2C STEMMA QT Rotary Encoder Breakout
- USB cable
- Mounting bracket (L-bracket or 3D print)

## Wiring Diagram
```
Rotary Encoder → ESP32
─────────────────────────
SDA (Blue)   → GPIO 21
SCL (Yellow) → GPIO 22
VCC (Red)    → 3.3V
GND (Black)  → GND
```

## Physical Installation

### Attaching to Cable Wheel (Center Mount)

Direct shaft coupling
1. Identify the center shaft/axle of the cable wheel
2. Use a shaft coupler (6mm to encoder shaft size)
3. Secure encoder to frame with L-bracket
4. Ensure encoder shaft rotates with wheel


## Setup Instructions

### 1. First Upload (Calibration)

The code comes with `CALIBRATION_MODE = true` by default.

1. Upload code to ESP32
2. Open Serial Monitor (115200 baud)
3. Follow on-screen instructions:
   - Row exactly 10 strokes
   - Press encoder button when done
4. **Write down the calibration value shown**

### 2. Update Calibration

In `src/main.cpp`:

**Line 9:** Update with your calibration value
```cpp
float METERS_PER_ROTATION = 2.354;  // Use YOUR value here
```

**Line 15:** Disable calibration mode
```cpp
bool CALIBRATION_MODE = false;
```

### 2. Re-upload & Use

1. Save changes
2. Upload again
3. Open Serial Monitor
4. Start rowing!

## Features

- Distance tracking (meters)  
- Calorie estimation  
- Session duration  
- Stroke counting  
- Strokes per minute  
- Max speed tracking  
- Auto-start/stop (30s inactivity)  
- CSV export format  

## Sample Output
```
╔════════════════════════════════════════════════════╗
║              SESSION COMPLETE!                     ║
╠════════════════════════════════════════════════════╣
║     Duration:        5 min 42 sec                  ║
║     Distance:        285.3 meters                   ║
║     Calories:        34.2 kcal                      ║
║     Total Strokes:   95                             ║
║     Avg per Stroke:  3.00 meters                    ║
║     Strokes/Minute:  16.7                            ║
║.    Max Speed:       4.25 m/s                       ║
╚════════════════════════════════════════════════════╝
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| "Encoder not found" | Check I2C wiring (pins 21, 22, 3.3V, GND) |
| No strokes detected | Lower `STROKE_THRESHOLD` to `6` |
| Too many fake strokes | Increase `STROKE_THRESHOLD` to `20` |
| Distance way off | Re-run calibration |
| Session ends too fast | Change `INACTIVITY_TIMEOUT` to `60000` |

## Fine-Tuning

### Adjust Calorie Calculation
```cpp
const float CALORIES_PER_METER = 0.15;  // Higher = more calories
```

### Adjust Auto-Stop Timer
```cpp
const unsigned long INACTIVITY_TIMEOUT = 60000;  // 60 seconds
```

## Project Structure
```
RowingMachine/
├── platformio.ini          # PlatformIO config
├── src/
│   └── main.cpp           # Main code
└── README.md              # This file
```

## Version History

- v1.0 - Initial release with terminal output only