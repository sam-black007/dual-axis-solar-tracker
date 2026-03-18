/*
 * ================================================================
 * DUAL AXIS SOLAR TRACKER - Arduino UNO Controller
 * ================================================================
 * 
 * Project by: Sam Joseph (@sam-black007) & K M Sri Hari (@kmsrihari123)
 * 
 * Features:
 * - Dual servo control for horizontal and vertical axis
 * - LDR sensor array for sun tracking
 * - 16x2 I2C LCD display
 * - Rotary encoder for manual control
 * - Physical button for menu navigation
 * - Serial communication with ESP32
 * 
 * Developed by: K M Sri Hari
 * - Hardware Design & Circuit Implementation
 * - Arduino UNO Motor Controller Programming
 * - LDR Sensor Integration
 * 
 * Required Libraries:
 * - Servo (built-in)
 * - Wire (built-in)
 * - EEPROM (built-in)
 * - LiquidCrystal_I2C (Frank de Brabander)
 * - Encoder (Paul Stoffregen)
 * 
 * Wiring:
 * - Servo H  → Pin 9
 * - Servo V  → Pin 10
 * - Encoder CLK → Pin 2
 * - Encoder DT  → Pin 3
 * - Encoder SW  → Pin 4 (INPUT_PULLUP)
 * - LDR TL → A0
 * - LDR TR → A1
 * - LDR BL → A2
 * - LDR BR → A3
 * - LCD 16x2 I2C (0x27) → SDA=A4, SCL=A5
 * ================================================================
 */

#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Encoder.h>
#include <EEPROM.h>

// ================================================================
// HARDWARE PINS
// ================================================================
#define BUTTON_PIN    4
#define LDR_TL        A0
#define LDR_TR        A1
#define LDR_BL        A2
#define LDR_BR        A3
#define SERVO_H_PIN   9
#define SERVO_V_PIN   10

// ================================================================
// HARDWARE INITIALIZATION
// ================================================================
Servo servoH, servoV;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Encoder rotaryEncoder(2, 3);

// ================================================================
// EEPROM ADDRESSES
// ================================================================
#define EEPROM_HOME_H     0
#define EEPROM_HOME_V     2
#define EEPROM_MAGIC      4
#define EEPROM_FLIP_H     5
#define EEPROM_FLIP_V     6
#define MAGIC_VALUE       0xAB

// ================================================================
// SERVO CONFIGURATION
// ================================================================
#define H_MIN_ANGLE      5
#define H_MAX_ANGLE      175
#define V_MIN_ANGLE      5
#define V_MAX_ANGLE      175

int homeH = 175;
int homeV = 5;
int currentH, currentV;
int targetH, targetV;
int moveSpeed = 8;

// ================================================================
// LDR SENSOR CONFIGURATION
// ================================================================
float emaTL = 512, emaTR = 512, emaBL = 512, emaBR = 512;
int rawTL = 0, rawTR = 0, rawBL = 0, rawBR = 0;

#define EMA_ALPHA        0.12f
#define H_TOLERANCE      110
#define V_TOLERANCE      70

bool flipH = false;
bool flipV = false;

// ================================================================
// SYSTEM STATE
// ================================================================
float sensorTemp = 0;
float sensorHum = 0;
float sensorPres = 0;
float sensorIrr = 0;

char wifiIP[20] = "No WiFi";
char wifiStatus[4] = "---";

enum OperatingMode {
    MODE_AUTO,
    MODE_MANUAL,
    MODE_DEMO_H,
    MODE_DEMO_V,
    MODE_DEMO_HV,
    MODE_PRESET,
    MODE_CUSTOM
};

OperatingMode currentMode = MODE_AUTO;

enum DisplayPage {
    PAGE_MAIN_MENU,
    PAGE_MANUAL,
    PAGE_DEMO,
    PAGE_PRESETS,
    PAGE_CUSTOM,
    PAGE_WEATHER,
    PAGE_TELEMETRY
};

DisplayPage currentPage = PAGE_MAIN_MENU;
DisplayPage pageStack[4];
uint8_t pageStackTop = 0;

// ================================================================
// DEMO MODE
// ================================================================
bool demoForward = true;
unsigned long demoTimer = 0;
#define DEMO_STEP_MS   18

// ================================================================
// SUN PRESETS (Based on Chennai, India location)
// ================================================================
const int16_t presetH[6] = {175, 140, 90, 40, 5, 90};
const int16_t presetV[6] = {5, 75, 150, 75, 5, 5};
const char* presetName[6] = {"Morning", "Forenoon", "Noon", "Afternoon", "Evening", "Night"};

// ================================================================
// CUSTOM WAYPOINTS
// ================================================================
struct Waypoint {
    int16_t h;
    int16_t v;
};

Waypoint waypoints[5];
uint8_t waypointCount = 0;
uint8_t waypointIndex = 0;
bool waypointRunning = false;
unsigned long waypointTimer = 0;
#define WAYPOINT_DWELL_MS   3000

// ================================================================
// MENU NAVIGATION
// ================================================================
int8_t menuIndex = 0;
bool editingH = true;
long encoderPosition = -999;
unsigned long buttonPressTime = 0;
bool buttonWasPressed = false;

// ================================================================
// LCD CACHE
// ================================================================
char lcdLine0[17] = "";
char lcdLine1[17] = "";
unsigned long lcdUpdateTime = 0;
#define LCD_UPDATE_MS   500

// ================================================================
// TIMING
// ================================================================
unsigned long trackingTimer = 0;
unsigned long ldrSendTimer = 0;
unsigned long liveUpdateTimer = 0;
#define TRACKING_INTERVAL_MS   160
#define LDR_SEND_INTERVAL_MS  600
#define LIVE_UPDATE_MS         600

// ================================================================
// SERIAL BUFFER
// ================================================================
char serialBuffer[100];
uint8_t serialBufferLen = 0;

// ================================================================
// EEPROM FUNCTIONS
// ================================================================
void eepromWriteInt(int address, int value) {
    EEPROM.write(address, highByte(value));
    EEPROM.write(address + 1, lowByte(value));
}

int eepromReadInt(int address) {
    return (int)(EEPROM.read(address) << 8) | EEPROM.read(address + 1);
}

void saveHomePosition() {
    eepromWriteInt(EEPROM_HOME_H, homeH);
    eepromWriteInt(EEPROM_HOME_V, homeV);
    EEPROM.write(EEPROM_MAGIC, MAGIC_VALUE);
}

void loadSettings() {
    if (EEPROM.read(EEPROM_MAGIC) == MAGIC_VALUE) {
        homeH = constrain(eepromReadInt(EEPROM_HOME_H), H_MIN_ANGLE, H_MAX_ANGLE);
        homeV = constrain(eepromReadInt(EEPROM_HOME_V), V_MIN_ANGLE, V_MAX_ANGLE);
        flipH = EEPROM.read(EEPROM_FLIP_H);
        flipV = EEPROM.read(EEPROM_FLIP_V);
    } else {
        homeH = 175;
        homeV = 5;
        flipH = false;
        flipV = false;
        saveHomePosition();
    }
}

void saveFlipSettings() {
    EEPROM.write(EEPROM_FLIP_H, flipH ? 1 : 0);
    EEPROM.write(EEPROM_FLIP_V, flipV ? 1 : 0);
}

// ================================================================
// LCD FUNCTIONS
// ================================================================
void lcdPadString(char* dest, const char* src) {
    uint8_t i = 0;
    while (i < 16 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    while (i < 16) {
        dest[i++] = ' ';
    }
    dest[16] = '\0';
}

void lcdUpdate(const char* line0, const char* line1) {
    char tmp0[17], tmp1[17];
    lcdPadString(tmp0, line0);
    lcdPadString(tmp1, line1);
    
    if (strncmp(tmp0, lcdLine0, 16) != 0) {
        lcd.setCursor(0, 0);
        lcd.print(tmp0);
        strncpy(lcdLine0, tmp0, 16);
    }
    
    if (strncmp(tmp1, lcdLine1, 16) != 0) {
        lcd.setCursor(0, 1);
        lcd.print(tmp1);
        strncpy(lcdLine1, tmp1, 16);
    }
}

void lcdSendNow() {
    lcdUpdateTime = 0;
}

void lcdShow(const char* line0, const char* line1) {
    char tmp0[17], tmp1[17];
    lcdPadString(tmp0, line0);
    lcdPadString(tmp1, line1);
    lcdUpdate(tmp0, tmp1);
    lcdSendNow();
}

void sendLCDToESP() {
    if (millis() - lcdUpdateTime < LCD_UPDATE_MS) return;
    lcdUpdateTime = millis();
    Serial.print("LCD:");
    Serial.print(lcdLine0);
    Serial.print('|');
    Serial.println(lcdLine1);
}

// ================================================================
// SERVO CONTROL
// ================================================================
void smoothServoMove() {
    if (millis() - trackingTimer < (unsigned long)moveSpeed) return;
    trackingTimer = millis();
    
    if (currentH != targetH || currentV != targetV) {
        if (currentH < targetH) currentH++;
        else if (currentH > targetH) currentH--;
        
        if (currentV < targetV) currentV++;
        else if (currentV > targetV) currentV--;
        
        servoH.write(currentH);
        servoV.write(currentV);
    }
}

void goHomePosition() {
    targetH = homeH;
    targetV = homeV;
}

// ================================================================
// LDR TRACKING
// ================================================================
void runLDRTracking() {
    if (millis() - trackingTimer < TRACKING_INTERVAL_MS) return;
    
    // Read LDR values with averaging
    int32_t sumTL = 0, sumTR = 0, sumBL = 0, sumBR = 0;
    for (uint8_t i = 0; i < 4; i++) {
        sumTL += analogRead(LDR_TL);
        sumTR += analogRead(LDR_TR);
        sumBL += analogRead(LDR_BL);
        sumBR += analogRead(LDR_BR);
        delayMicroseconds(400);
    }
    
    // Apply EMA filter
    emaTL = EMA_ALPHA * (sumTL / 4.0f) + (1.0f - EMA_ALPHA) * emaTL;
    emaTR = EMA_ALPHA * (sumTR / 4.0f) + (1.0f - EMA_ALPHA) * emaTR;
    emaBL = EMA_ALPHA * (sumBL / 4.0f) + (1.0f - EMA_ALPHA) * emaBL;
    emaBR = EMA_ALPHA * (sumBR / 4.0f) + (1.0f - EMA_ALPHA) * emaBR;
    
    rawTL = (int)emaTL;
    rawTR = (int)emaTR;
    rawBL = (int)emaBL;
    rawBR = (int)emaBR;
    
    // Calculate differential
    int diffH = (rawTL + rawBL) / 2 - (rawTR + rawBR) / 2;
    int diffV = (rawTL + rawTR) / 2 - (rawBL + rawBR) / 2;
    
    // Determine step size based on difference magnitude
    uint8_t stepH = (abs(diffH) > 250) ? 3 : (abs(diffH) > 130) ? 2 : 1;
    uint8_t stepV = (abs(diffV) > 250) ? 3 : (abs(diffV) > 130) ? 2 : 1;
    
    // Adjust target position
    if (abs(diffH) > H_TOLERANCE) {
        bool lightLeft = (diffH > 0);
        if (flipH) lightLeft = !lightLeft;
        if (lightLeft) {
            targetH = max(H_MIN_ANGLE, targetH - stepH);
        } else {
            targetH = min(H_MAX_ANGLE, targetH + stepH);
        }
    }
    
    if (abs(diffV) > V_TOLERANCE) {
        bool lightTop = (diffV > 0);
        if (flipV) lightTop = !lightTop;
        if (lightTop) {
            targetV = min(V_MAX_ANGLE, targetV + stepV);
        } else {
            targetV = max(V_MIN_ANGLE, targetV - stepV);
        }
    }
    
    // Send LDR data to ESP32 periodically
    if (millis() - ldrSendTimer > LDR_SEND_INTERVAL_MS) {
        ldrSendTimer = millis();
        Serial.print("LDR:");
        Serial.print(rawTL);
        Serial.print(',');
        Serial.print(rawTR);
        Serial.print(',');
        Serial.print(rawBL);
        Serial.print(',');
        Serial.println(rawBR);
    }
}

// ================================================================
// DEMO MODE
// ================================================================
void runDemoMode() {
    if (millis() - demoTimer < DEMO_STEP_MS) return;
    demoTimer = millis();
    
    switch (currentMode) {
        case MODE_DEMO_H:
            targetV = 90;
            if (demoForward) {
                targetH = min(targetH + 1, H_MAX_ANGLE);
                if (targetH >= H_MAX_ANGLE) demoForward = false;
            } else {
                targetH = max(targetH - 1, H_MIN_ANGLE);
                if (targetH <= H_MIN_ANGLE) demoForward = true;
            }
            break;
            
        case MODE_DEMO_V:
            targetH = 90;
            if (demoForward) {
                targetV = min(targetV + 1, V_MAX_ANGLE);
                if (targetV >= V_MAX_ANGLE) demoForward = false;
            } else {
                targetV = max(targetV - 1, V_MIN_ANGLE);
                if (targetV <= V_MIN_ANGLE) demoForward = true;
            }
            break;
            
        case MODE_DEMO_HV:
            if (demoForward) {
                targetH = min(targetH + 1, H_MAX_ANGLE);
                targetV = min(targetV + 1, V_MAX_ANGLE);
                if (targetH >= H_MAX_ANGLE && targetV >= V_MAX_ANGLE) demoForward = false;
            } else {
                targetH = max(targetH - 1, H_MIN_ANGLE);
                targetV = max(targetV - 1, V_MIN_ANGLE);
                if (targetH <= H_MIN_ANGLE && targetV <= V_MIN_ANGLE) demoForward = true;
            }
            break;
            
        default:
            break;
    }
}

// ================================================================
// CUSTOM PATH / WAYPOINTS
// ================================================================
void runCustomPath() {
    if (!waypointRunning || waypointCount == 0) return;
    
    // Wait for servo to reach waypoint
    if (currentH == targetH && currentV == targetV) {
        if (millis() - waypointTimer < WAYPOINT_DWELL_MS) return;
        
        waypointIndex++;
        if (waypointIndex >= waypointCount) {
            waypointRunning = false;
            waypointIndex = 0;
            currentMode = MODE_MANUAL;
            goHomePosition();
            lcdShow("Custom Path", "Complete - Home");
            delay(2000);
            return;
        }
        
        targetH = waypoints[waypointIndex].h;
        targetV = waypoints[waypointIndex].v;
        waypointTimer = millis();
    }
}

// ================================================================
// SERIAL COMMAND PARSER
// ================================================================
float getSerialFloat(const char* key) {
    char* pos = strstr(serialBuffer, key);
    if (!pos) return 0;
    return atof(pos + strlen(key));
}

void parseSerialCommand() {
    if (serialBufferLen == 0) return;
    
    // Parse sensor data from ESP32
    if (serialBuffer[0] == 'T' && serialBuffer[1] == ':') {
        sensorTemp = getSerialFloat("T:");
        sensorHum = getSerialFloat(",H:");
        sensorPres = getSerialFloat(",P:");
        sensorIrr = getSerialFloat(",I:");
        return;
    }
    
    // WiFi status
    if (strncmp(serialBuffer, "WIFI:", 5) == 0) {
        if (strcmp(serialBuffer, "WIFI:FAIL") == 0) {
            strcpy(wifiStatus, "---");
            strcpy(wifiIP, "No WiFi");
        } else {
            strcpy(wifiStatus, "OK");
            strncpy(wifiIP, serialBuffer + 5, 19);
            wifiIP[19] = '\0';
            lcdShow("WiFi Connected!", wifiIP);
            delay(2000);
        }
        return;
    }
    
    // Servo horizontal
    if (strncmp(serialBuffer, "SH:", 3) == 0) {
        targetH = constrain(atoi(serialBuffer + 3), H_MIN_ANGLE, H_MAX_ANGLE);
        currentMode = MODE_MANUAL;
        currentPage = PAGE_MANUAL;
        editingH = true;
        Serial.print("OK:SH:");
        Serial.println(targetH);
        return;
    }
    
    // Servo vertical
    if (strncmp(serialBuffer, "SV:", 3) == 0) {
        targetV = constrain(atoi(serialBuffer + 3), V_MIN_ANGLE, V_MAX_ANGLE);
        currentMode = MODE_MANUAL;
        currentPage = PAGE_MANUAL;
        editingH = false;
        Serial.print("OK:SV:");
        Serial.println(targetV);
        return;
    }
    
    // Movement speed
    if (strncmp(serialBuffer, "SPD:", 4) == 0) {
        moveSpeed = constrain(atoi(serialBuffer + 4), 2, 50);
        Serial.print("OK:SPD:");
        Serial.println(moveSpeed);
        return;
    }
    
    // Flip axis
    if (strcmp(serialBuffer, "FLIP:H") == 0) {
        flipH = !flipH;
        saveFlipSettings();
        Serial.println("OK:FLIP:H");
        return;
    }
    
    if (strcmp(serialBuffer, "FLIP:V") == 0) {
        flipV = !flipV;
        saveFlipSettings();
        Serial.println("OK:FLIP:V");
        return;
    }
    
    // Go home
    if (strcmp(serialBuffer, "HOME") == 0) {
        goHomePosition();
        currentMode = MODE_MANUAL;
        Serial.println("OK:HOME");
        return;
    }
    
    // Set home position
    if (strncmp(serialBuffer, "SETHOME:", 8) == 0) {
        char* comma = strchr(serialBuffer + 8, ',');
        if (comma) {
            homeH = constrain(atoi(serialBuffer + 8), H_MIN_ANGLE, H_MAX_ANGLE);
            homeV = constrain(atoi(comma + 1), V_MIN_ANGLE, V_MAX_ANGLE);
            saveHomePosition();
            char tmp[17];
            snprintf(tmp, 17, "H:%d V:%d", homeH, homeV);
            lcdShow("Home Saved!", tmp);
            delay(1000);
        }
        return;
    }
    
    // Mode changes
    if (strcmp(serialBuffer, "MODE:AUTO") == 0) {
        currentMode = MODE_AUTO;
        Serial.println("OK:AUTO");
        return;
    }
    
    if (strcmp(serialBuffer, "MODE:MANUAL") == 0) {
        currentMode = MODE_MANUAL;
        Serial.println("OK:MANUAL");
        return;
    }
    
    if (strcmp(serialBuffer, "MODE:DEMO_H") == 0) {
        currentMode = MODE_DEMO_H;
        demoForward = true;
        targetH = H_MIN_ANGLE;
        Serial.println("OK:DEMO_H");
        return;
    }
    
    if (strcmp(serialBuffer, "MODE:DEMO_V") == 0) {
        currentMode = MODE_DEMO_V;
        demoForward = true;
        targetV = V_MIN_ANGLE;
        Serial.println("OK:DEMO_V");
        return;
    }
    
    if (strcmp(serialBuffer, "MODE:DEMO_HV") == 0) {
        currentMode = MODE_DEMO_HV;
        demoForward = true;
        targetH = H_MIN_ANGLE;
        targetV = V_MIN_ANGLE;
        Serial.println("OK:DEMO_HV");
        return;
    }
    
    // Preset
    if (strncmp(serialBuffer, "PRESET:", 7) == 0) {
        uint8_t idx = constrain(atoi(serialBuffer + 7), 0, 5);
        targetH = presetH[idx];
        targetV = presetV[idx];
        currentMode = MODE_PRESET;
        char tmp[17];
        snprintf(tmp, 17, "H:%d V:%d", targetH, targetV);
        lcdShow(presetName[idx], tmp);
        delay(1500);
        goHomePosition();
        currentMode = MODE_MANUAL;
        Serial.print("OK:PRESET:");
        Serial.println(presetName[idx]);
        return;
    }
    
    // Custom waypoints
    if (strcmp(serialBuffer, "CUSTOM:CLEAR") == 0) {
        waypointCount = 0;
        Serial.println("OK:CLEAR");
        return;
    }
    
    if (strcmp(serialBuffer, "CUSTOM:START") == 0) {
        if (waypointCount > 0) {
            waypointRunning = true;
            waypointIndex = 0;
            targetH = waypoints[0].h;
            targetV = waypoints[0].v;
            waypointTimer = millis();
            currentMode = MODE_CUSTOM;
            Serial.println("OK:START");
        } else {
            Serial.println("ERR:NO_WP");
        }
        return;
    }
    
    if (strncmp(serialBuffer, "CUSTOM:ADD:", 11) == 0) {
        char* comma = strchr(serialBuffer + 11, ',');
        if (comma && waypointCount < 5) {
            waypoints[waypointCount].h = constrain(atoi(serialBuffer + 11), H_MIN_ANGLE, H_MAX_ANGLE);
            waypoints[waypointCount].v = constrain(atoi(comma + 1), V_MIN_ANGLE, V_MAX_ANGLE);
            waypointCount++;
            Serial.print("OK:ADD:");
            Serial.println(waypointCount);
        }
        return;
    }
}

void readSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            serialBuffer[serialBufferLen] = '\0';
            parseSerialCommand();
            serialBufferLen = 0;
        } else if (c != '\r' && serialBufferLen < 99) {
            serialBuffer[serialBufferLen++] = c;
        }
    }
}

// ================================================================
// ENCODER AND BUTTON HANDLING
// ================================================================
void handleEncoderButton() {
    long newPos = rotaryEncoder.read() / 4;
    if (newPos != encoderPosition) {
        int delta = (newPos > encoderPosition) ? 1 : -1;
        encoderPosition = newPos;
        
        if (currentPage == PAGE_MANUAL) {
            if (editingH) {
                targetH = constrain(targetH + delta * 2, H_MIN_ANGLE, H_MAX_ANGLE);
            } else {
                targetV = constrain(targetV + delta * 2, V_MIN_ANGLE, V_MAX_ANGLE);
            }
        } else {
            menuIndex += delta;
            menuIndex = constrain(menuIndex, 0, getMenuMax());
        }
    }
    
    bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (buttonPressed && !buttonWasPressed) {
        buttonPressTime = millis();
        buttonWasPressed = true;
    }
    
    if (!buttonPressed && buttonWasPressed) {
        unsigned long pressDuration = millis() - buttonPressTime;
        if (pressDuration > 700) {
            navigateBack();
        } else if (pressDuration > 40) {
            handleSelect();
        }
        buttonWasPressed = false;
    }
}

int getMenuMax() {
    switch (currentPage) {
        case PAGE_MAIN_MENU: return 6;
        case PAGE_MANUAL: return 2;
        case PAGE_DEMO: return 3;
        case PAGE_PRESETS: return 6;
        case PAGE_CUSTOM: return waypointCount + 2;
        default: return 0;
    }
}

void navigateBack() {
    if (pageStackTop > 0) {
        currentPage = pageStack[--pageStackTop];
    } else {
        currentPage = PAGE_MAIN_MENU;
    }
    menuIndex = 0;
    updateDisplay();
}

void pushPage(DisplayPage page) {
    if (pageStackTop < 4) {
        pageStack[pageStackTop++] = currentPage;
    }
    currentPage = page;
    menuIndex = 0;
    updateDisplay();
}

// ================================================================
// MENU HANDLING
// ================================================================
void handleSelect() {
    switch (currentPage) {
        case PAGE_MAIN_MENU:
            switch (menuIndex) {
                case 0:
                    currentMode = MODE_AUTO;
                    lcdShow("Auto Track ON", "LDR Tracking...");
                    break;
                case 1:
                    currentMode = MODE_MANUAL;
                    editingH = true;
                    pushPage(PAGE_MANUAL);
                    break;
                case 2:
                    pushPage(PAGE_DEMO);
                    break;
                case 3:
                    pushPage(PAGE_PRESETS);
                    break;
                case 4:
                    pushPage(PAGE_CUSTOM);
                    break;
                case 5:
                    pushPage(PAGE_WEATHER);
                    break;
                case 6:
                    pushPage(PAGE_TELEMETRY);
                    break;
            }
            break;
            
        case PAGE_MANUAL:
            if (menuIndex == 0) {
                editingH = true;
            } else if (menuIndex == 1) {
                editingH = false;
            } else {
                navigateBack();
            }
            break;
            
        case PAGE_DEMO:
            switch (menuIndex) {
                case 0:
                    currentMode = MODE_DEMO_H;
                    demoForward = true;
                    targetH = H_MIN_ANGLE;
                    break;
                case 1:
                    currentMode = MODE_DEMO_V;
                    demoForward = true;
                    targetV = V_MIN_ANGLE;
                    break;
                case 2:
                    currentMode = MODE_DEMO_HV;
                    demoForward = true;
                    targetH = H_MIN_ANGLE;
                    targetV = V_MIN_ANGLE;
                    break;
                case 3:
                    currentMode = MODE_MANUAL;
                    goHomePosition();
                    navigateBack();
                    break;
            }
            break;
            
        case PAGE_PRESETS:
            if (menuIndex < 6) {
                targetH = presetH[menuIndex];
                targetV = presetV[menuIndex];
                currentMode = MODE_PRESET;
                char tmp[17];
                snprintf(tmp, 17, "H:%d V:%d", targetH, targetV);
                lcdShow(presetName[menuIndex], tmp);
                unsigned long waitStart = millis();
                while ((currentH != targetH || currentV != targetV) && millis() - waitStart < 10000) {
                    smoothServoMove();
                }
                delay(1500);
                goHomePosition();
                currentMode = MODE_MANUAL;
                lcdShow("Done", "Returned Home");
            } else {
                navigateBack();
            }
            break;
            
        case PAGE_CUSTOM:
            if (menuIndex < (int)waypointCount) {
                char tmp[17];
                snprintf(tmp, 17, "WP%d H:%d V:%d", menuIndex + 1, waypoints[menuIndex].h, waypoints[menuIndex].v);
                lcdShow(tmp, "");
                delay(1200);
            } else if (menuIndex == (int)waypointCount && waypointCount < 5) {
                waypoints[waypointCount].h = targetH;
                waypoints[waypointCount].v = targetV;
                waypointCount++;
                char tmp[17];
                snprintf(tmp, 17, "WP%d Added!", waypointCount);
                lcdShow(tmp, "");
                delay(1000);
            } else {
                if (waypointCount > 0) {
                    waypointRunning = true;
                    waypointIndex = 0;
                    targetH = waypoints[0].h;
                    targetV = waypoints[0].v;
                    waypointTimer = millis();
                    currentMode = MODE_CUSTOM;
                }
                navigateBack();
            }
            break;
            
        case PAGE_WEATHER:
        case PAGE_TELEMETRY:
            navigateBack();
            break;
            
        default:
            navigateBack();
            break;
    }
}

// ================================================================
// DISPLAY UPDATE
// ================================================================
void updateDisplay() {
    char line0[17], line1[17];
    char modeStr[5];
    
    // Get mode string
    switch (currentMode) {
        case MODE_AUTO: strcpy(modeStr, "AUTO"); break;
        case MODE_MANUAL: strcpy(modeStr, "MAN"); break;
        case MODE_DEMO_H: strcpy(modeStr, "DEMH"); break;
        case MODE_DEMO_V: strcpy(modeStr, "DEMV"); break;
        case MODE_DEMO_HV: strcpy(modeStr, "DEM"); break;
        case MODE_PRESET: strcpy(modeStr, "PRESET"); break;
        case MODE_CUSTOM: strcpy(modeStr, "CUSTOM"); break;
        default: strcpy(modeStr, "---"); break;
    }
    
    switch (currentPage) {
        case PAGE_MAIN_MENU: {
            const char* items[] = {"Auto Track", "Manual", "Demo", "Presets", "Custom", "Weather", "Telemetry"};
            snprintf(line0, 17, ">%s", items[min((int)menuIndex, 6)]);
            snprintf(line1, 17, " %s", items[min((int)menuIndex + 1, 6)]);
            break;
        }
        
        case PAGE_MANUAL:
            snprintf(line0, 17, "%cH:%3d %cV:%3d", editingH ? '>' : '_', targetH, editingH ? '_' : '>', targetV);
            snprintf(line1, 17, "%s Btn=sw LP=bk", editingH ? "[H]" : "[V]");
            break;
            
        case PAGE_DEMO: {
            const char* demoItems[] = {"H Sweep", "V Sweep", "H+V Both", "Stop+Home"};
            snprintf(line0, 17, ">%s", demoItems[min((int)menuIndex, 3)]);
            snprintf(line1, 17, " %s", demoItems[min((int)menuIndex + 1, 3)]);
            break;
        }
        
        case PAGE_PRESETS:
            if (menuIndex < 6) {
                snprintf(line0, 17, ">%s", presetName[menuIndex]);
                snprintf(line1, 17, "H:%d V:%d", presetH[menuIndex], presetV[menuIndex]);
            } else {
                strcpy(line0, ">< Back");
                strcpy(line1, "");
            }
            break;
            
        case PAGE_CUSTOM:
            if (waypointCount == 0) {
                strcpy(line0, "No WPs yet");
                strcpy(line1, "Rotate=add");
            } else if (menuIndex < (int)waypointCount) {
                snprintf(line0, 17, "WP%d H:%3d", menuIndex + 1, waypoints[menuIndex].h);
                snprintf(line1, 17, "    V:%3d", waypoints[menuIndex].v);
            } else if (menuIndex == (int)waypointCount) {
                snprintf(line0, 17, ">Add H:%d V:%d", targetH, targetV);
                strcpy(line1, "");
            } else {
                snprintf(line0, 17, ">Run %d WPs", waypointCount);
                strcpy(line1, "< Back");
            }
            break;
            
        case PAGE_WEATHER: {
            static uint8_t subPage = 0;
            static unsigned long subTimer = 0;
            if (millis() - subTimer > 1800) {
                subPage = (subPage + 1) % 3;
                subTimer = millis();
            }
            
            switch (subPage) {
                case 0:
                    snprintf(line0, 17, "T:%.1fC H:%.0f%%", sensorTemp, sensorHum);
                    snprintf(line1, 17, "P:%.0fhPa", sensorPres);
                    break;
                case 1:
                    snprintf(line0, 17, "Irr:%.0fW/m2", sensorIrr);
                    snprintf(line1, 17, "WiFi:%s", wifiStatus);
                    break;
                case 2:
                    snprintf(line0, 17, "WiFi:%s", wifiStatus);
                    snprintf(line1, 17, "%.16s", wifiIP);
                    break;
            }
            break;
        }
        
        case PAGE_TELEMETRY: {
            static uint8_t subPage = 0;
            static unsigned long subTimer = 0;
            if (millis() - subTimer > 1500) {
                subPage = (subPage + 1) % 3;
                subTimer = millis();
            }
            
            switch (subPage) {
                case 0:
                    snprintf(line0, 17, "H:%3ddeg V:%3ddeg", currentH, currentV);
                    snprintf(line1, 17, "Mode:%s", modeStr);
                    break;
                case 1:
                    snprintf(line0, 17, "T:%.1fC H:%.0f%%", sensorTemp, sensorHum);
                    snprintf(line1, 17, "P:%.0fhPa", sensorPres);
                    break;
                case 2:
                    snprintf(line0, 17, "TL:%d TR:%d", rawTL, rawTR);
                    snprintf(line1, 17, "BL:%d BR:%d", rawBL, rawBR);
                    break;
            }
            break;
        }
    }
    
    lcdUpdate(line0, line1);
}

// ================================================================
// SETUP
// ================================================================
void setup() {
    Serial.begin(9600);
    
    // Initialize servos
    servoH.attach(SERVO_H_PIN);
    servoV.attach(SERVO_V_PIN);
    
    // Load saved settings
    loadSettings();
    
    // Set initial positions
    currentH = homeH;
    currentV = homeV;
    targetH = homeH;
    targetV = homeV;
    servoH.write(currentH);
    servoV.write(currentV);
    
    // Initialize LCD
    Wire.begin();
    delay(100);
    lcd.init();
    delay(50);
    lcd.init();
    lcd.backlight();
    lcd.clear();
    delay(50);
    
    // Initialize button pin
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Show startup message
    char tmp[17];
    snprintf(tmp, 17, "Home H%d V%d", homeH, homeV);
    lcdShow("SOLAR TRACKER", tmp);
    delay(2000);
    
    currentPage = PAGE_MAIN_MENU;
    updateDisplay();
}

// ================================================================
// MAIN LOOP
// ================================================================
void loop() {
    // Handle serial communication
    readSerial();
    
    // Handle encoder and button
    handleEncoderButton();
    
    // Smooth servo movement
    smoothServoMove();
    
    // Run based on current mode
    switch (currentMode) {
        case MODE_AUTO:
            runLDRTracking();
            break;
        case MODE_DEMO_H:
        case MODE_DEMO_V:
        case MODE_DEMO_HV:
            runDemoMode();
            break;
        case MODE_CUSTOM:
            runCustomPath();
            break;
        default:
            break;
    }
    
    // Periodic updates
    unsigned long now = millis();
    
    // Update display for dynamic pages
    if (now - liveUpdateTimer > LIVE_UPDATE_MS) {
        liveUpdateTimer = now;
        
        if (currentPage == PAGE_WEATHER || currentPage == PAGE_TELEMETRY) {
            updateDisplay();
        }
        
        if (currentPage == PAGE_MANUAL) {
            static int prevH = -1, prevV = -1;
            if (currentH != prevH || currentV != prevV) {
                prevH = currentH;
                prevV = currentV;
                updateDisplay();
            }
        }
        
        if (currentPage == PAGE_DEMO) {
            static int prevH = -1, prevV = -1;
            if (currentH != prevH || currentV != prevV) {
                prevH = currentH;
                prevV = currentV;
                updateDisplay();
            }
        }
        
        // Send position to ESP32
        Serial.print("POS:");
        Serial.print(currentH);
        Serial.print(',');
        Serial.println(currentV);
    }
    
    // Send LCD content periodically
    sendLCDToESP();
}
