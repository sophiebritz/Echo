Wiring guide · MDCopyROWING MACHINE WITH TFT DISPLAY - SETUP GUIDE
🔌 HARDWARE WIRING
ESP32 to ILI9486 3.5" TFT (8-bit Parallel)
The MCUFRIEND_kbv library auto-detects pins on ESP32. Standard wiring:
TFT Pin    → ESP32 Pin
─────────────────────────
LCD_D0     → GPIO 12
LCD_D1     → GPIO 13
LCD_D2     → GPIO 26
LCD_D3     → GPIO 25
LCD_D4     → GPIO 17
LCD_D5     → GPIO 16
LCD_D6     → GPIO 27
LCD_D7     → GPIO 14

LCD_RD     → GPIO 2
LCD_WR     → GPIO 4
LCD_RS/DC  → GPIO 15
LCD_CS     → GPIO 33
LCD_RST    → GPIO 32

LED/BL     → 3.3V (backlight - use 100Ω resistor)
VCC        → 5V
GND        → GND
Alternative: If your display has a different pinout or the above doesn't work, you can manually define pins by editing the library files.

Rotary Encoder (I2C)
Encoder    → ESP32
───────────────────
VIN        → 3.3V
GND        → GND
SDA        → GPIO 21
SCL        → GPIO 22

LED Ring (WS2812)
LED Ring   → ESP32
───────────────────
DIN        → GPIO 23
VCC        → 5V
GND        → GND

📚 REQUIRED LIBRARIES
Install these via Arduino Library Manager or PlatformIO:
inilib_deps = 
    adafruit/Adafruit seesaw Library@^1.7.7
    adafruit/Adafruit BusIO@^1.16.1
    fastled/FastLED@^3.7.0
    adafruit/Adafruit GFX Library@^1.11.0
    prenticedavid/MCUFRIEND_kbv@^3.0.0

⚙️ PLATFORMIO.INI
ini[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps = 
    adafruit/Adafruit seesaw Library@^1.7.7
    adafruit/Adafruit BusIO@^1.16.1
    fastled/FastLED@^3.7.0
    adafruit/Adafruit GFX Library@^1.11.0
    prenticedavid/MCUFRIEND_kbv@^3.0.0

🎨 DISPLAY FEATURES
Calibration Screen

Shows "CALIBRATING" message
Displays stroke count
Animated water at bottom

Rowing Screen

Boat animation - moves left to right based on distance
Waving red flag on boat mast
Checkered finish line appears at 400m+
Animated water waves

Stats Display (Top of Screen)

Large distance counter (cyan)
Stroke count (green)
Time elapsed (yellow)
Current speed (white)
Calories burned
Strokes per minute (SPM)
Max speed

Summary Screen (End of Session)

"SESSION COMPLETE!" banner
Boat at finish line
Complete session statistics
Auto-returns to idle after 5 seconds


🚀 QUICK START

Wire everything according to diagram above
Install libraries via PlatformIO or Arduino IDE
Upload code to ESP32
Power on - should see "Initializing..." on screen
Calibration - Do 3 strokes (counts automatically)
Start rowing! - Boat moves, stats update


🎯 CALIBRATION
The system auto-calibrates on first use:

Does 3 sample strokes
Calculates average stroke length
Configures LED ring to match your stroke
Switches to running mode

No manual configuration needed!

🎮 CONTROLS

Start rowing → Session auto-starts
Stop rowing for 30 seconds → Session auto-ends
Stats → Updated live on screen every 100ms
LEDs → Fill based on stroke progress, color by speed


🐛 TROUBLESHOOTING
Screen is white/blank

Check all data pins D0-D7 are connected
Check VCC is 5V, GND connected
Try pressing ESP32 reset button

Screen shows wrong colors

Check if it's ILI9488 instead of ILI9486
Change line in code: if (ID == 0xD3D3) ID = 0x9488;

Encoder not found

Check I2C wiring (GPIO 21/22)
Check encoder is powered (3.3V)
Run I2C scanner to verify address 0x36

LEDs not working

Check DIN connected to GPIO 23
Try 5V instead of 3.3V for LED power
Check GND is common between ESP32 and LEDs

Boat doesn't move

Encoder is working but stroke detection might need tuning
Watch Serial Monitor for stroke counts
Adjust STROKE_PAUSE_MS if needed (currently 300ms)


📊 CUSTOMIZATION
Adjust finish line distance
cppif (currentSession.distance > 400) {  // Change 400 to desired meters
Change boat speed mapping
cppint maxDistance = 500;  // Screen width = 500 meters
Adjust colors
cpp#define SKYBLUE 0x867D  // Change to any 16-bit color
Bigger text
cpptft.setTextSize(4);  // Increase from current sizes

🎨 BOAT ANIMATION DETAILS
The boat:

Brown hull (triangle + rectangle)
Black mast (vertical line)
White sail (triangle)
Red flag (animated wave effect)
Moves smoothly based on distance

The waves:

Sine wave animation
Two layers (cyan + sky blue)
Continuously animating


🏁 FINISH LINE
Appears when distance > 400m:

6x6 checkered pattern
Black pole
Fixed position at x=460


Enjoy your rowing machine! 🚣‍♀️