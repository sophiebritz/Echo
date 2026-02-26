/*
 * ROWING MACHINE - GOAL-BASED SYSTEM
 * 
 * Features:
 * - EC11 Rotary Encoder for menu navigation (turn to select, press to confirm)
 * - Adafruit Seesaw I2C Encoder for rowing stroke detection
 * - Three workout modes: Distance, Calories, Time
 * - Automatic calibration (learns stroke length in 3 strokes)
 * - LED pace feedback: Red (too slow/fast), Green (good), Orange (transition)
 * - TFT display with animated boat showing progress
 * - Inactivity detection (20 sec warning, 60 sec shutoff)
 * - Exit confirmation dialog
 * 
 * Hardware:
 * - ESP32 or Arduino Mega
 * - EC11 Rotary Encoder (CLK→32, DT→33, SW→25)
 * - Adafruit Seesaw I2C Encoder (SDA→21, SCL→22, Addr 0x36)
 * - MCUFRIEND 3.5" TFT Display (shield pinout)
 * - WS2812 LED Ring 12 LEDs (DI→23)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <FastLED.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>

// ╔════════════════════════════════════════════════════════════╗
// ║                   PIN CONFIGURATION                       ║
// ╚════════════════════════════════════════════════════════════╝

// I2C Seesaw Encoder (Rowing Detection)
#define SDA_PIN          21 // [cite: 3]
#define SCL_PIN          22 // [cite: 3]
#define SEESAW_ADDR      0x36 // [cite: 2]

// EC11 Menu Encoder (Reassigned to avoid TFT conflict)
#define EC11_CLK_PIN     34 
#define EC11_DT_PIN      35 
#define EC11_SW_PIN      39 

// LED Ring
#define LED_DATA_PIN     23 // [cite: 2]
#define NUM_LEDS         12 // [cite: 11]
#define LED_BRIGHTNESS   40 // [cite: 2]

// TFT Control Pins (Manual Parallel Wiring)
#define LCD_RST  32
#define LCD_CS   33
#define LCD_RS   15
#define LCD_WR    4
#define LCD_RD    2

// ╔════════════════════════════════════════════════════════════╗
// ║                   CONFIGURATION                           ║
// ╚════════════════════════════════════════════════════════════╝

// Physical constants
const float DRUM_DIAMETER_MM   = 90.0f;
const int   ENCODER_RESOLUTION = 20;
const float ROPE_PER_PULSE_M   = (DRUM_DIAMETER_MM * PI) / ENCODER_RESOLUTION / 1000.0f;
const float CALORIES_PER_METER = 0.12f;

// Calibration
const int   CALIBRATION_STROKES = 3;
const float FULL_RING_AT_FRACTION = 0.85f;

// Stroke detection
const unsigned long STROKE_PAUSE_MS = 300;
const int32_t MIN_STROKE_PULSES = 6;

// Speed tracking
const float SPEED_SMOOTHING = 0.88f;
const float SPEED_MIN_DT_SEC = 0.02f;

// Pace zones for LED feedback
const float PACE_TOO_SLOW = 0.8f;  // m/s
const float PACE_GOOD_MIN = 1.0f;  // m/s
const float PACE_GOOD_MAX = 2.0f;  // m/s
const float PACE_TOO_FAST = 2.5f;  // m/s

// Inactivity
const unsigned long INACTIVITY_WARNING = 20000;  // 20 seconds
const unsigned long INACTIVITY_SHUTOFF = 60000;  // 60 seconds

// Display
const int BORDER_WIDTH = 4;  // 4mm black border on left

// ╔════════════════════════════════════════════════════════════╗
// ║                    HARDWARE OBJECTS                       ║
// ╚════════════════════════════════════════════════════════════╝

CRGB leds[NUM_LEDS];
Adafruit_seesaw seesawEncoder;  // I2C encoder for rowing
MCUFRIEND_kbv tft;

// Colors
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define YELLOW  0xFFE0
#define WHITE   0xFFFF
#define BROWN   0x8200
#define DARKBLUE 0x0010
#define ORANGE  0xFD20
#define GRAY    0x8410

// ╔════════════════════════════════════════════════════════════╗
// ║                    STATE MACHINE                          ║
// ╚════════════════════════════════════════════════════════════╝

enum State {
  STATE_OFF,
  STATE_HOME,
  STATE_SELECT_MODE,
  STATE_SELECT_GOAL,
  STATE_CALIBRATING,
  STATE_READY,
  STATE_ROWING,
  STATE_PAUSED,
  STATE_COMPLETE,
  STATE_CONFIRM_EXIT
};

enum WorkoutMode {
  MODE_DISTANCE,
  MODE_CALORIES,
  MODE_TIME
};

State currentState = STATE_HOME;
WorkoutMode selectedMode = MODE_DISTANCE;
bool needsCalibration = true;

// Goal values
float goalDistance = 100.0f;  // meters
float goalCalories = 50.0f;   // kcal
unsigned long goalTime = 300; // seconds (5 min)

// Session data
struct SessionData {
  float distance;
  float calories;
  unsigned long duration;
  int strokes;
  float maxSpeed;
  float avgSpeed;
  unsigned long startTime;
};

SessionData session = {};

// Calibration
float calibratedStrokeMeters = 0.0f;
float metersPerLED = 0.0f;
int calibStrokeCount = 0;
float calibSumStrokeMeters = 0.0f;

// EC11 Encoder State (menu control)
volatile int ec11Position = 0;
bool lastEC11ButtonState = HIGH;
unsigned long lastEC11ButtonTime = 0;
const unsigned long EC11_DEBOUNCE = 50;

// Seesaw Encoder Tracking (rowing detection)
int32_t lastStrokePos = 0;

// Stroke detection
bool inStroke = false;
int32_t strokePulses = 0;
unsigned long lastMovementTime = 0;

// Speed tracking
float filteredSpeed = 0.0f;
unsigned long lastSpeedTime = 0;
int32_t lastSpeedPos = 0;
float peakSpeedThisStroke = 0.0f;
float speedSum = 0.0f;
int speedCount = 0;

// Activity tracking
unsigned long lastActivity = 0;
bool warningShown = false;

// Menu navigation
int menuSelection = 0;
int maxMenuItems = 3;

// Display
bool screenNeedsRedraw = true;

// ╔════════════════════════════════════════════════════════════╗
// ║                    EC11 INTERRUPT HANDLER                 ║
// ╚════════════════════════════════════════════════════════════╝

void IRAM_ATTR ec11ISR() {
  static unsigned long lastInterruptTime = 0;
  unsigned long interruptTime = millis();
  
  // Debounce
  if (interruptTime - lastInterruptTime > 5) {
    if (digitalRead(EC11_DT_PIN) == HIGH) {
      ec11Position++;  // Clockwise
    } else {
      ec11Position--;  // Counter-clockwise
    }
  }
  lastInterruptTime = interruptTime;
}

// ╔════════════════════════════════════════════════════════════╗
// ║                    HELPER FUNCTIONS                       ║
// ╚════════════════════════════════════════════════════════════╝

float pulsesToMeters(int32_t pulses) {
  return (float)pulses * ROPE_PER_PULSE_M;
}

void clearLEDs() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

// Check EC11 button press (for menu navigation)
bool isEC11ButtonPressed() {
  bool currentState = digitalRead(EC11_SW_PIN);
  unsigned long now = millis();
  
  if (currentState == LOW && lastEC11ButtonState == HIGH) {
    if (now - lastEC11ButtonTime > EC11_DEBOUNCE) {
      lastEC11ButtonTime = now;
      lastEC11ButtonState = LOW;
      return true;
    }
  }
  
  if (currentState == HIGH) {
    lastEC11ButtonState = HIGH;
  }
  
  return false;
}

// Get EC11 encoder delta (for menu navigation)
int getEC11Delta() {
  int delta = ec11Position;
  ec11Position = 0;  // Reset after reading
  return delta;
}

// Get Seesaw encoder position (for rowing)
int32_t getSeesawPosition() {
  return seesawEncoder.getEncoderPosition();
}

void updatePaceLEDs() {
  // LED feedback based on current speed
  CRGB color;
  
  if (filteredSpeed < PACE_TOO_SLOW) {
    // Too slow - flash red
    color = ((millis() / 500) % 2) ? CRGB::Red : CRGB::Black;
  } 
  else if (filteredSpeed >= PACE_GOOD_MIN && filteredSpeed <= PACE_GOOD_MAX) {
    // Good pace - solid green
    color = CRGB::Green;
  } 
  else if (filteredSpeed > PACE_TOO_FAST) {
    // Too fast - flash red quickly
    color = ((millis() / 300) % 2) ? CRGB::Red : CRGB::Black;
  } 
  else {
    // Transitioning - orange
    color = CRGB::Orange;
  }
  
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

// ╔════════════════════════════════════════════════════════════╗
// ║                    DISPLAY FUNCTIONS                      ║
// ╚════════════════════════════════════════════════════════════╝

void drawBorder() {
  // 4mm black border on left
  tft.fillRect(0, 0, BORDER_WIDTH, 320, BLACK);
}

void drawBoat(int x, int y, int scale) {
  // Scalable boat
  int w = 40 * scale / 100;
  int h = 25 * scale / 100;
  
  tft.fillTriangle(x, y + h/2, x + w, y + h/2, x + w/2, y, BROWN);
  tft.fillRect(x + w/8, y + h/2, w*3/4, h/2, BROWN);
  tft.fillRect(x + w/2, y - h, 2, h, BLACK);
  tft.fillTriangle(x + w/2 + 2, y - h, x + w/2 + 2, y, x + w, y - h/2, WHITE);
  tft.fillRect(x + w/2 + 2, y - h - 10, 10, 5, RED);
}

void drawHomeScreen() {
  tft.fillScreen(BLACK);
  drawBorder();
  
  // Large, friendly welcome
  tft.setTextColor(WHITE);
  tft.setTextSize(6);
  tft.setCursor(80, 60);
  tft.print("ROWING");
  
  tft.setTextSize(3);
  tft.setCursor(100, 140);
  tft.print("EXERCISE");
  
  // Simple boat graphic
  drawBoat(200, 200, 120);
  
  // Clear instruction in box
  tft.fillRoundRect(60, 250, 360, 50, 10, GREEN);
  tft.setTextColor(BLACK);
  tft.setTextSize(3);
  tft.setCursor(90, 263);
  tft.print("PRESS to START");
  
  screenNeedsRedraw = false;
}

void drawModeSelection() {
  tft.fillScreen(BLACK);
  drawBorder();
  
  // Large title
  tft.setTextColor(CYAN);
  tft.setTextSize(4);
  tft.setCursor(90, 20);
  tft.print("CHOOSE TYPE");
  
  const char* modes[] = {"DISTANCE", "CALORIES", "TIME"};
  
  for (int i = 0; i < 3; i++) {
    int y = 80 + i * 70;
    
    if (i == menuSelection) {
      // Large highlighted box
      tft.fillRoundRect(BORDER_WIDTH + 30, y - 5, 410, 60, 8, WHITE);
      tft.setTextColor(BLACK);
      tft.setTextSize(5);
      tft.setCursor(80, y + 5);
      tft.print(modes[i]);
    } else {
      // Unselected - smaller, dimmer
      tft.setTextColor(GRAY);
      tft.setTextSize(3);
      tft.setCursor(100, y + 12);
      tft.print(modes[i]);
    }
  }
  
  // Simple instruction
  tft.fillRoundRect(60, 270, 360, 35, 8, DARKBLUE);
  tft.setTextColor(CYAN);
  tft.setTextSize(2);
  tft.setCursor(110, 278);
  tft.print("TURN - PRESS");
  
  screenNeedsRedraw = false;
}

void drawGoalSelection() {
  tft.fillScreen(BLACK);
  drawBorder();
  
  // Title
  tft.setTextColor(CYAN);
  tft.setTextSize(4);
  tft.setCursor(120, 20);
  tft.print("SET GOAL");
  
  // Show mode
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(140, 70);
  
  switch(selectedMode) {
    case MODE_DISTANCE:
      tft.print("Meters to row");
      break;
    case MODE_CALORIES:
      tft.print("Calories to burn");
      break;
    case MODE_TIME:
      tft.print("Minutes to row");
      break;
  }
  
  // HUGE goal value in center
  tft.fillRoundRect(BORDER_WIDTH + 40, 110, 400, 110, 12, DARKBLUE);
  tft.drawRoundRect(BORDER_WIDTH + 40, 110, 400, 110, 12, CYAN);
  
  tft.setTextColor(YELLOW);
  tft.setTextSize(12);
  
  int cursorX = 120;
  switch(selectedMode) {
    case MODE_DISTANCE:
      if (goalDistance >= 1000) cursorX = 90;
      else if (goalDistance >= 100) cursorX = 110;
      tft.setCursor(cursorX, 130);
      tft.print((int)goalDistance);
      tft.setTextSize(6);
      tft.print("m");
      break;
    case MODE_CALORIES:
      if (goalCalories >= 100) cursorX = 100;
      tft.setCursor(cursorX, 130);
      tft.print((int)goalCalories);
      tft.setTextSize(5);
      tft.print("kcal");
      break;
    case MODE_TIME:
      int mins = goalTime / 60;
      int secs = goalTime % 60;
      tft.setCursor(100, 130);
      tft.print(mins);
      tft.setTextSize(8);
      tft.print(":");
      tft.setTextSize(12);
      if (secs < 10) tft.print("0");
      tft.print(secs);
      break;
  }
  
  // Instructions in box
  tft.fillRoundRect(60, 250, 360, 50, 8, GREEN);
  tft.setTextColor(BLACK);
  tft.setTextSize(3);
  tft.setCursor(90, 263);
  tft.print("TURN - PRESS");
  
  screenNeedsRedraw = false;
}

void drawCalibrationScreen() {
  tft.fillScreen(BLUE);
  drawBorder();
  
  tft.setTextColor(YELLOW);
  tft.setTextSize(3);
  tft.setCursor(60, 80);
  tft.print("CALIBRATING");
  
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(80, 140);
  tft.print("Do ");
  tft.print(CALIBRATION_STROKES);
  tft.print(" strokes");
  
  tft.setCursor(120, 180);
  tft.print("Stroke ");
  tft.print(calibStrokeCount);
  tft.print("/");
  tft.print(CALIBRATION_STROKES);
  
  screenNeedsRedraw = false;
}

void drawReadyScreen() {
  tft.fillScreen(DARKBLUE);
  drawBorder();
  
  drawBoat(50, 200, 100);
  
  tft.setTextColor(WHITE);
  tft.setTextSize(5);
  tft.setCursor(140, 60);
  tft.print("READY!");
  
  tft.setTextColor(CYAN);
  tft.setTextSize(3);
  tft.setCursor(60, 140);
  tft.print("Goal: ");
  
  switch(selectedMode) {
    case MODE_DISTANCE:
      tft.print((int)goalDistance);
      tft.print("m");
      break;
    case MODE_CALORIES:
      tft.print((int)goalCalories);
      tft.print(" kcal");
      break;
    case MODE_TIME:
      int m = goalTime / 60;
      int s = goalTime % 60;
      tft.print(m);
      tft.print(":");
      if (s < 10) tft.print("0");
      tft.print(s);
      break;
  }
  
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 250);
  tft.print("Start rowing!");
  
  screenNeedsRedraw = false;
}

void drawRowingScreen() {
  static unsigned long lastUpdate = 0;
  static int lastBoatX = 50;
  static float lastDisplayValue = -1;
  
  if (screenNeedsRedraw) {
    tft.fillScreen(DARKBLUE);
    drawBorder();
    lastDisplayValue = -1;
    screenNeedsRedraw = false;
  }
  
  // Calculate progress percentage
  float progress = 0.0f;
  float currentValue = 0.0f;
  
  switch(selectedMode) {
    case MODE_DISTANCE:
      currentValue = session.distance;
      progress = session.distance / goalDistance;
      break;
    case MODE_CALORIES:
      currentValue = session.calories;
      progress = session.calories / goalCalories;
      break;
    case MODE_TIME:
      currentValue = session.duration;
      progress = (float)session.duration / (float)goalTime;
      break;
  }
  
  if (progress > 1.0f) progress = 1.0f;
  
  // Update stats every 500ms
  if (millis() - lastUpdate > 500) {
    // Clear and redraw main stat
    if (abs(currentValue - lastDisplayValue) > 0.5) {
      tft.fillRect(BORDER_WIDTH + 20, 20, 430, 100, BLACK);
      
      tft.setTextColor(CYAN);
      tft.setTextSize(10);
      tft.setCursor(80, 30);
      
      switch(selectedMode) {
        case MODE_DISTANCE:
          tft.print((int)session.distance);
          tft.setTextSize(4);
          tft.print(" m");
          break;
        case MODE_CALORIES:
          tft.print((int)session.calories);
          tft.setTextSize(4);
          tft.print(" kcal");
          break;
        case MODE_TIME:
          int m = session.duration / 60;
          int s = session.duration % 60;
          tft.print(m);
          tft.setTextSize(6);
          tft.print(":");
          tft.setTextSize(10);
          if (s < 10) tft.print("0");
          tft.print(s);
          break;
      }
      
      lastDisplayValue = currentValue;
    }
    
    lastUpdate = millis();
  }
  
  // Boat position based on progress
  int newBoatX = 50 + (int)(progress * 350);
  if (newBoatX > 410) newBoatX = 410;
  
  if (abs(newBoatX - lastBoatX) > 5) {
    // Clear old boat
    tft.fillRect(lastBoatX - 10, 150, 80, 80, DARKBLUE);
    // Draw new boat
    drawBoat(newBoatX, 200, 100);
    lastBoatX = newBoatX;
  }
  
  // Goal line at end
  tft.fillRect(460, 150, 10, 100, YELLOW);
}

void drawCompleteScreen() {
  tft.fillScreen(BLACK);
  drawBorder();
  
  tft.setTextColor(YELLOW);
  tft.setTextSize(4);
  tft.setCursor(80, 20);
  tft.print("COMPLETE!");
  
  drawBoat(200, 100, 120);
  
  tft.setTextColor(GREEN);
  tft.setTextSize(2);
  
  int y = 170;
  
  tft.setCursor(50, y);
  tft.print("Distance: ");
  tft.print(session.distance, 1);
  tft.print(" m");
  
  tft.setCursor(50, y + 30);
  tft.print("Calories: ");
  tft.print(session.calories, 1);
  tft.print(" kcal");
  
  tft.setCursor(50, y + 60);
  int m = session.duration / 60;
  int s = session.duration % 60;
  tft.print("Time: ");
  tft.print(m);
  tft.print(":");
  if (s < 10) tft.print("0");
  tft.print(s);
  
  tft.setCursor(50, y + 90);
  tft.print("Strokes: ");
  tft.print(session.strokes);
  
  if (speedCount > 0) {
    tft.setCursor(50, y + 120);
    tft.print("Avg Speed: ");
    tft.print(speedSum / speedCount, 1);
    tft.print(" m/s");
  }
  
  screenNeedsRedraw = false;
}

void drawConfirmExit() {
  tft.fillRect(100, 100, 280, 120, ORANGE);
  tft.drawRect(100, 100, 280, 120, WHITE);
  
  tft.setTextColor(BLACK);
  tft.setTextSize(3);
  tft.setCursor(140, 120);
  tft.print("EXIT?");
  
  tft.setTextSize(2);
  if (menuSelection == 0) {
    tft.fillRect(130, 160, 80, 30, WHITE);
    tft.setTextColor(BLACK);
    tft.setCursor(150, 165);
    tft.print("YES");
    tft.setTextColor(WHITE);
    tft.fillRect(240, 160, 80, 30, ORANGE);
    tft.setCursor(265, 165);
    tft.print("NO");
  } else {
    tft.fillRect(130, 160, 80, 30, ORANGE);
    tft.setTextColor(WHITE);
    tft.setCursor(150, 165);
    tft.print("YES");
    tft.fillRect(240, 160, 80, 30, WHITE);
    tft.setTextColor(BLACK);
    tft.setCursor(265, 165);
    tft.print("NO");
  }
}

// ╔════════════════════════════════════════════════════════════╗
// ║                    STATE HANDLERS                         ║
// ╚════════════════════════════════════════════════════════════╝

void handleHomeState() {
  if (screenNeedsRedraw) {
    drawHomeScreen();
  }
  
  if (isEC11ButtonPressed()) {
    currentState = STATE_SELECT_MODE;
    menuSelection = (int)selectedMode;
    screenNeedsRedraw = true;
  }
}

void handleModeSelection() {
  if (screenNeedsRedraw) {
    drawModeSelection();
  }
  
  int delta = getEC11Delta();
  if (delta != 0) {
    menuSelection += (delta > 0) ? 1 : -1;
    if (menuSelection < 0) menuSelection = 2;
    if (menuSelection > 2) menuSelection = 0;
    screenNeedsRedraw = true;
  }
  
  if (isEC11ButtonPressed()) {
    selectedMode = (WorkoutMode)menuSelection;
    currentState = STATE_SELECT_GOAL;
    screenNeedsRedraw = true;
  }
}

void handleGoalSelection() {
  if (screenNeedsRedraw) {
    drawGoalSelection();
  }
  
  int delta = getEC11Delta();
  if (delta != 0) {
    switch(selectedMode) {
      case MODE_DISTANCE:
        goalDistance += (delta > 0) ? 5 : -5;
        if (goalDistance < 5) goalDistance = 5;
        if (goalDistance > 10000) goalDistance = 10000;
        break;
      case MODE_CALORIES:
        goalCalories += (delta > 0) ? 10 : -10;
        if (goalCalories < 10) goalCalories = 10;
        if (goalCalories > 1000) goalCalories = 1000;
        break;
      case MODE_TIME:
        goalTime += (delta > 0) ? 30 : -30;
        if (goalTime < 30) goalTime = 30;
        if (goalTime > 7200) goalTime = 7200;
        break;
    }
    screenNeedsRedraw = true;
  }
  
  if (isEC11ButtonPressed()) {
    if (needsCalibration) {
      currentState = STATE_CALIBRATING;
      calibStrokeCount = 0;
      calibSumStrokeMeters = 0.0f;
    } else {
      currentState = STATE_READY;
    }
    screenNeedsRedraw = true;
  }
}

void handleCalibration() {
  if (screenNeedsRedraw) {
    drawCalibrationScreen();
  }
  
  // Handled in main loop stroke detection
}

void finishCalibration() {
  calibratedStrokeMeters = calibSumStrokeMeters / (float)CALIBRATION_STROKES;
  float fullRingMeters = calibratedStrokeMeters * FULL_RING_AT_FRACTION;
  metersPerLED = fullRingMeters / (float)NUM_LEDS;
  
  needsCalibration = false;
  
  // Flash LEDs
  for (int i = 0; i < 3; i++) {
    fill_solid(leds, NUM_LEDS, CRGB::Blue);
    FastLED.show();
    delay(200);
    clearLEDs();
    delay(200);
  }
  
  currentState = STATE_READY;
  screenNeedsRedraw = true;
}

void handleReadyState() {
  if (screenNeedsRedraw) {
    drawReadyScreen();
  }
  
  // Wait for movement to start
}

void startSession() {
  session = {};
  session.startTime = millis();
  lastActivity = millis();
  seesawEncoder.setEncoderPosition(0);
  lastStrokePos = 0;
  filteredSpeed = 0.0f;
  speedSum = 0.0f;
  speedCount = 0;
  
  currentState = STATE_ROWING;
  screenNeedsRedraw = true;
}

void handleRowingState() {
  drawRowingScreen();
  updatePaceLEDs();
  
  // Check button for exit
  if (isEC11ButtonPressed()) {
    currentState = STATE_CONFIRM_EXIT;
    menuSelection = 1;  // Default to NO
    drawConfirmExit();
  }
  
  // Check if goal reached
  bool goalReached = false;
  switch(selectedMode) {
    case MODE_DISTANCE:
      if (session.distance >= goalDistance) goalReached = true;
      break;
    case MODE_CALORIES:
      if (session.calories >= goalCalories) goalReached = true;
      break;
    case MODE_TIME:
      if (session.duration >= goalTime) goalReached = true;
      break;
  }
  
  if (goalReached) {
    currentState = STATE_COMPLETE;
    clearLEDs();
    screenNeedsRedraw = true;
    delay(500);
  }
}

void handleConfirmExit() {
  int delta = getEC11Delta();
  if (delta != 0) {
    menuSelection = 1 - menuSelection;  // Toggle 0/1
    drawConfirmExit();
  }
  
  if (isEC11ButtonPressed()) {
    if (menuSelection == 0) {
      // YES - exit
      currentState = STATE_COMPLETE;
      clearLEDs();
      screenNeedsRedraw = true;
    } else {
      // NO - resume
      currentState = STATE_ROWING;
      screenNeedsRedraw = true;
    }
  }
}

void handleCompleteState() {
  static unsigned long completeTime = 0;
  
  if (screenNeedsRedraw) {
    drawCompleteScreen();
    completeTime = millis();
  }
  
  // Auto-return after 30 seconds or button press
  if (isEC11ButtonPressed() || (millis() - completeTime > 30000)) {
    currentState = STATE_HOME;
    screenNeedsRedraw = true;
  }
}

// ╔════════════════════════════════════════════════════════════╗
// ║                    SETUP                                  ║
// ╚════════════════════════════════════════════════════════════╝

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // EC11 Menu Encoder Setup
  pinMode(EC11_CLK_PIN, INPUT_PULLUP);
  pinMode(EC11_DT_PIN, INPUT_PULLUP);
  pinMode(EC11_SW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(EC11_CLK_PIN), ec11ISR, FALLING);
  
  // TFT
  uint16_t ID = tft.readID();
  if (ID == 0xD3D3) ID = 0x9486;
  tft.begin(ID);
  tft.setRotation(1);
  tft.fillScreen(BLACK);
  
  // LEDs
  FastLED.addLeds<WS2812, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  clearLEDs();
  
  // I2C Seesaw Encoder (for rowing)
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!seesawEncoder.begin(SEESAW_ADDR)) {
    tft.fillScreen(RED);
    tft.setTextColor(WHITE);
    tft.setTextSize(3);
    tft.setCursor(100, 150);
    tft.print("ROWING ENCODER ERROR!");
    while (1) delay(1000);
  }
  
  seesawEncoder.enableEncoderInterrupt();
  
  currentState = STATE_HOME;
  screenNeedsRedraw = true;
  
  Serial.println("Rowing Machine Ready!");
  Serial.println("EC11: Menu navigation");
  Serial.println("Seesaw: Rowing detection");
}

// ╔════════════════════════════════════════════════════════════╗
// ║                    MAIN LOOP                              ║
// ╚════════════════════════════════════════════════════════════╝

void loop() {
  unsigned long now = millis();
  
  // Update session duration if rowing
  if (currentState == STATE_ROWING) {
    session.duration = (now - session.startTime) / 1000;
  }
  
  // Handle Seesaw encoder movement during rowing
  if (currentState == STATE_ROWING || currentState == STATE_CALIBRATING || currentState == STATE_READY) {
    int32_t currentPos = getSeesawPosition();
    
    if (currentPos != lastStrokePos) {
      int32_t delta = currentPos - lastStrokePos;
      
      if (delta > 0) {
        strokePulses += delta;
        inStroke = true;
        lastMovementTime = now;
        lastActivity = now;
        
        // Start session if in ready state
        if (currentState == STATE_READY) {
          startSession();
        }
        
        // Speed calculation
        float dt = (now - lastSpeedTime) / 1000.0f;
        if (dt >= SPEED_MIN_DT_SEC) {
          int32_t dPulses = abs(currentPos - lastSpeedPos);
          float rawSpeed = pulsesToMeters(dPulses) / dt;
          filteredSpeed = SPEED_SMOOTHING * filteredSpeed + (1.0f - SPEED_SMOOTHING) * rawSpeed;
          lastSpeedTime = now;
          lastSpeedPos = currentPos;
          
          if (filteredSpeed > peakSpeedThisStroke) {
            peakSpeedThisStroke = filteredSpeed;
          }
          
          // Track for average
          speedSum += filteredSpeed;
          speedCount++;
        }
        
        // Update session data
        if (currentState == STATE_ROWING) {
          session.distance += pulsesToMeters(delta);
          session.calories = session.distance * CALORIES_PER_METER;
        }
      }
      
      lastStrokePos = currentPos;
    }
    
    // Stroke end detection
    if (inStroke && (now - lastMovementTime >= STROKE_PAUSE_MS)) {
      if (strokePulses >= MIN_STROKE_PULSES) {
        float strokeMeters = pulsesToMeters(strokePulses);
        
        if (currentState == STATE_CALIBRATING) {
          calibStrokeCount++;
          calibSumStrokeMeters += strokeMeters;
          screenNeedsRedraw = true;
          
          if (calibStrokeCount >= CALIBRATION_STROKES) {
            finishCalibration();
          }
        } else if (currentState == STATE_ROWING) {
          session.strokes++;
          if (peakSpeedThisStroke > session.maxSpeed) {
            session.maxSpeed = peakSpeedThisStroke;
          }
        }
      }
      
      inStroke = false;
      strokePulses = 0;
      peakSpeedThisStroke = 0.0f;
    }
  }
  
  // Inactivity check
  if (currentState == STATE_READY || currentState == STATE_ROWING) {
    if (now - lastActivity > INACTIVITY_WARNING && !warningShown) {
      // Show "Still using?" dialog
      tft.fillRect(100, 100, 280, 80, ORANGE);
      tft.drawRect(100, 100, 280, 80, WHITE);
      tft.setTextColor(BLACK);
      tft.setTextSize(3);
      tft.setCursor(120, 120);
      tft.print("STILL USING?");
      warningShown = true;
    }
    
    if (now - lastActivity > INACTIVITY_SHUTOFF) {
      currentState = STATE_HOME;
      screenNeedsRedraw = true;
      warningShown = false;
    }
  } else {
    warningShown = false;
  }
  
  // State machine
  switch (currentState) {
    case STATE_HOME:
      handleHomeState();
      break;
    case STATE_SELECT_MODE:
      handleModeSelection();
      break;
    case STATE_SELECT_GOAL:
      handleGoalSelection();
      break;
    case STATE_CALIBRATING:
      handleCalibration();
      break;
    case STATE_READY:
      handleReadyState();
      break;
    case STATE_ROWING:
      handleRowingState();
      break;
    case STATE_CONFIRM_EXIT:
      handleConfirmExit();
      break;
    case STATE_COMPLETE:
      handleCompleteState();
      break;
  }
  
  delay(10);
}
