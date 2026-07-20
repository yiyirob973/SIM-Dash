/* ====================================================================================
 *  SIM RACING GAUGE CLUSTER CONTROLLER (Arduino Nano)
 *  
 *  MODULAR KEY-VALUE (TOKEN) FORMAT:
 *  The code scans for explicit string identifiers separated by semicolons (;).
 *  Items can arrive in any order or be omitted entirely. Unsent tokens retain their
 *  last valid state or fall back cleanly without crashing the loop.
 *
 *  Tokens: 
 *  S= (ShiftLight), E= (EngineLight), V= (Velocity/Speed), R= (RPM), 
 *  T= (Temp), F= (Fuel), B= (Boost), L1= (LCD Line 1), L2= (LCD Line 2)
 *
 *  Expected Stream Example: 
 *  "S=1;V=60;R=4500;B=12;L1=BOOST ACTIVE;L2=CRUISING;\n"
 * ==================================================================================== */

#include <LiquidCrystal_I2C.h> 
#include <Wire.h>              
#include <TimerOne.h>          

// ==========================================
// --- HARDWARE PIN CONFIGURATION ---
// ==========================================
#define shiftLight 3  // Native PWM
#define engineLight 4 // Digital Out
#define TACH 2        // Timer1 Interrupt (DDS)
#define TEMP 5        // Native PWM
#define SP 11         // Timer1 Interrupt (DDS)
#define FUEL 6        // Native PWM
#define BOOST 9       // Native PWM (NEW: Core Boost Gauge Pin)
#define FIVEV 12      // Auxiliary 5V Rail

// ==========================================
// --- SERIAL PARSING VARIABLES ---
// ==========================================
const byte maxChars = 140; // Expanded to handle extra boost tokens safely      
char receivedChars[maxChars];    
char tempChars[maxChars];        
boolean newData = false;         

// ==========================================
// --- TIMEOUT FAILSAFE VARIABLES ---
// ==========================================
unsigned long lastValidDataTime = 0;
const unsigned long timeoutMillis = 2000; 
bool isHardwareStandby = false;

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// --- TIMING VARIABLES ---
// ==========================================
unsigned long lastProcessTime = 0;
const unsigned long intervalMicros = 8333; // 120Hz refresh rate

int currentShiftPWM = 0; 

// Array to hold parsed telemetry data: [Shift, EngLight, Speed, RPM, Temp, Fuel, Boost]
int values[7] = {0, 0, 0, 0, 0, 0, 0}; 

// LCD screen buffers
char lcdLine1[17] = "                "; 
char lcdLine2[17] = "                "; 
char lastLine1[17] = ""; 
char lastLine2[17] = "";

// ==========================================
// --- ISR (INTERRUPT) FREQUENCY VARIABLES ---
// ==========================================
volatile unsigned int tachPeriodTicks = 0;
volatile unsigned int speedoPeriodTicks = 0;
volatile unsigned int tachTickCounter = 0;
volatile unsigned int speedoTickCounter = 0;
volatile bool tachState = false;
volatile bool speedoState = false;

// Direct Port Manipulation for maximum CPU efficiency
void toneISR() {
  if (tachPeriodTicks > 0) {
    tachTickCounter++;
    if (tachTickCounter >= tachPeriodTicks) {
      tachTickCounter = 0;
      tachState = !tachState; 
      if (tachState) { PORTD |= B00000100; }  
      else           { PORTD &= ~B00000100; } 
    }
  }

  if (speedoPeriodTicks > 0) {
    speedoTickCounter++;
    if (speedoTickCounter >= speedoPeriodTicks) {
      speedoTickCounter = 0;
      speedoState = !speedoState; 
      if (speedoState) { PORTB |= B00001000; }  
      else             { PORTB &= ~B00001000; } 
    }
  }
}

// ==========================================
// --- GAUGE CALIBRATION TABLES ---
// ==========================================
const int TEMP_POINTS = 3;
const int tempSteps[] = {0, 80, 140};    
const int tempPWMVal[] = {0, 170, 220};  

const int FUEL_POINTS = 4;
const int fuelSteps[] = {0, 15, 50, 100};   
const int fuelPWMVal[] = {0, 130, 161, 255};

// NEW: Boost Gauge Calibration (Example setup for 0 to 30 PSI/Bar equivalent metrics)
const int BOOST_POINTS = 3;
const int boostSteps[] = {0, 15, 30};    
const int boostPWMVal[] = {0, 127, 255};  

const int TACH_POINTS = 14;
const int rpmSteps[] = {0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000}; 
const int tachHz[] = {0,38,73,105,135,166,197,226,258,289,320,352,386,434};                   

const int SPEED_POINTS = 8;
const int speedSteps[] = {0,16,40,80,100,120,140,180}; 
const int speedHz[] = {0,31,88,178,222,265,300,384};   

int calcHz(int currentVal, const int stepsArray[], const int hzArray[], int totalPoints) {
  if (currentVal <= stepsArray[0]) return hzArray[0];
  if (currentVal >= stepsArray[totalPoints - 1]) return hzArray[totalPoints - 1];

  for (int i = 0; i < totalPoints - 1; i++) {
    if (currentVal >= stepsArray[i] && currentVal <= stepsArray[i+1]) {
      long gapValue = (long)hzArray[i] + ((long)(currentVal - stepsArray[i]) * (hzArray[i+1] - hzArray[i])) / (stepsArray[i+1] - stepsArray[i]);
      return (int)gapValue;
    }
  }
  return 0;
}

// ==========================================
// --- NON-BLOCKING SERIAL READ ---
// ==========================================
void recvWithEndMarker() {
    static byte ndx = 0;
    char endMarker = '\n';
    char rc;
   
    while (Serial.available() > 0 && newData == false) {
        rc = Serial.read();
        if (rc != endMarker) {
            if (rc != '\r' && ndx < maxChars - 1) { 
                receivedChars[ndx] = rc;
                ndx++;
            }
        } else {
            receivedChars[ndx] = '\0'; 
            ndx = 0;
            newData = true;
        }
    }
}

void extractAndPadString(char* source, char* destination) {
    if (source != NULL) {
        int len = strlen(source);
        for (int i = 0; i < 16; i++) {
            if (i < len) destination[i] = source[i];
            else destination[i] = ' '; 
        }
        destination[16] = '\0';
    } else {
        for (int i = 0; i < 16; i++) destination[i] = ' ';
        destination[16] = '\0';
    }
}

// ==========================================
// --- TOKEN-BASED KEY-VALUE PARSER ---
// ==========================================
bool parseKeyValData() {
    char * token;
    token = strtok(tempChars, ";");
    
    while (token != NULL) {
        if (strncmp(token, "S=", 2) == 0) { 
            values[0] = atoi(token + 2); 
        } 
        else if (strncmp(token, "E=", 2) == 0) { 
            values[1] = atoi(token + 2); 
        } 
        else if (strncmp(token, "V=", 2) == 0) { 
            values[2] = atoi(token + 2); 
        } 
        else if (strncmp(token, "R=", 2) == 0) { 
            values[3] = atoi(token + 2); 
        } 
        else if (strncmp(token, "T=", 2) == 0) { 
            values[4] = atoi(token + 2); 
        } 
        else if (strncmp(token, "F=", 2) == 0) { 
            values[5] = atoi(token + 2); 
        } 
        else if (strncmp(token, "B=", 2) == 0) { // NEW: Parse boost payload
            values[6] = atoi(token + 2); 
        }
        else if (strncmp(token, "L1=", 3) == 0) { 
            extractAndPadString(token + 3, lcdLine1); 
        } 
        else if (strncmp(token, "L2=", 3) == 0) { 
            extractAndPadString(token + 3, lcdLine2); 
        }
        token = strtok(NULL, ";");
    }
    return true; 
}

void triggerFailsafe() {
    for (int i = 0; i < 7; i++) { // Clear all 7 indices cleanly
        values[i] = 0; 
    }
    strcpy(lcdLine1, "  SIMHUB READY  ");
    strcpy(lcdLine2, " WAITING FOR PC ");
}

// ==========================================
// --- SYSTEM SETUP ---
// ==========================================
void setup() {
  Serial.begin(115200); 
  
  pinMode(FIVEV, OUTPUT);
  digitalWrite(FIVEV, HIGH); 
  
  pinMode(shiftLight, OUTPUT);
  pinMode(engineLight, OUTPUT);
  digitalWrite(shiftLight, LOW);
  digitalWrite(engineLight, LOW);

  pinMode(TACH, OUTPUT);
  pinMode(SP, OUTPUT);
  digitalWrite(TACH, LOW);
  digitalWrite(SP, LOW);
  
  pinMode(TEMP, OUTPUT);
  pinMode(FUEL, OUTPUT);
  pinMode(BOOST, OUTPUT); // NEW: Define Boost output behavior
  digitalWrite(TEMP, LOW);
  digitalWrite(FUEL, LOW);
  digitalWrite(BOOST, LOW);
    
  Timer1.initialize(50);
  Timer1.attachInterrupt(toneISR); 

  Wire.begin();
  lcd.init();
  Wire.setClock(400000); 
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("System Loading..");
}

// ==========================================
// --- MAIN PROCESSING LOOP ---
// ==========================================
void loop() {
  recvWithEndMarker();
  if (newData == true) {
    strcpy(tempChars, receivedChars); 
    if (parseKeyValData()) {
        lastValidDataTime = millis();
        isHardwareStandby = false;
    }
    newData = false;
  }

  if (!isHardwareStandby && (millis() - lastValidDataTime > timeoutMillis)) {
      triggerFailsafe();
      isHardwareStandby = true;
  }

  unsigned long nowMicros = micros();
  if (nowMicros - lastProcessTime >= intervalMicros) {
    lastProcessTime = nowMicros; 
                
    // Speedometer Logic
    int rawSpeedHz = calcHz(values[2], speedSteps, speedHz, SPEED_POINTS);
    static int currentSpeedHz = 0;
    static int zeroFrameCounter = 0;
    
    if (rawSpeedHz == 0 && currentSpeedHz > 15) {
      zeroFrameCounter++;
      if (zeroFrameCounter < 12) { rawSpeedHz = currentSpeedHz; }
    } else { zeroFrameCounter = 0; }

    const int SPEED_ACCEL_RATE = 5; 
    const int SPEED_DECEL_RATE = 3; 

    if (rawSpeedHz > currentSpeedHz + SPEED_ACCEL_RATE) { currentSpeedHz += SPEED_ACCEL_RATE; } 
    else if (rawSpeedHz < currentSpeedHz - SPEED_DECEL_RATE) { currentSpeedHz -= SPEED_DECEL_RATE; } 
    else { currentSpeedHz = rawSpeedHz; }

    // Tachometer Logic 
    int targetTachHz = calcHz(values[3], rpmSteps, tachHz, TACH_POINTS);

    // Analog Gauges Logic (Temp, Fuel, and Boost)
    int targetTempPWM = calcHz(values[4], tempSteps, tempPWMVal, TEMP_POINTS);
    int targetFuelPWM = calcHz(values[5], fuelSteps, fuelPWMVal, FUEL_POINTS);
    int targetBoostPWM = calcHz(values[6], boostSteps, boostPWMVal, BOOST_POINTS); // NEW: Calibrate Boost

    if (targetFuelPWM > 0) { analogWrite(FUEL, targetFuelPWM); } else { analogWrite(FUEL, 0); }
    if (targetTempPWM > 0) { analogWrite(TEMP, targetTempPWM); } else { analogWrite(TEMP, 0); }
    if (targetBoostPWM > 0) { analogWrite(BOOST, targetBoostPWM); } else { analogWrite(BOOST, 0); } // NEW: Direct Write
    
    // Frequency Delivery to Interrupt
    noInterrupts(); 
    if (targetTachHz > 15) { tachPeriodTicks = 10000 / targetTachHz; } 
    else { tachPeriodTicks = 0; tachState = false; digitalWrite(TACH, LOW); }
    
    if (currentSpeedHz > 15) { speedoPeriodTicks = 10000 / currentSpeedHz; } 
    else { speedoPeriodTicks = 0; speedoState = false; digitalWrite(SP, LOW); }
    interrupts(); 

    // Indicator Lights Logic
    if (values[1] == 1) { digitalWrite(engineLight, HIGH); } 
    else { digitalWrite(engineLight, LOW); }

    int targetShiftPWM = 0;
    if (values[0] == 1) { targetShiftPWM = 20; }       
    else if (values[0] == 2) { targetShiftPWM = 255; }  

    const int SHIFT_FADE_SPEED = 2; 
    if (currentShiftPWM < targetShiftPWM) {
        currentShiftPWM += SHIFT_FADE_SPEED;
        if (currentShiftPWM > targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    } else if (currentShiftPWM > targetShiftPWM) {
        currentShiftPWM -= (SHIFT_FADE_SPEED * 3); 
        if (currentShiftPWM < targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    }
    analogWrite(shiftLight, currentShiftPWM);

    // LCD Screen Updates
    if (strcmp(lcdLine1, lastLine1) != 0) {
      lcd.setCursor(0, 0); lcd.print(lcdLine1); strcpy(lastLine1, lcdLine1); 
    }
    if (strcmp(lcdLine2, lastLine2) != 0) {
      lcd.setCursor(0, 1); lcd.print(lcdLine2); strcpy(lastLine2, lcdLine2); 
    } 
  }
}
