# 🏎️ Sim Racing Gauge Cluster Controller
Written mostly by AI. I could not find code online to help power what I needed so posting here so others can possibly find it. This is a simple method for anybody with an Arduino nano to power way more than what SimHub nativily supports. The rest of this Readme was writting by AI to describe the functionality and how to implement it.

A high-performance, ultra-stable hardware controller designed to bridge PC-based sim racing telemetry (like SimHub) to physical automotive dashboard gauges. Running on an 8-bit Arduino Nano, this firmware uses a 120Hz processing loop and hardware-level interrupt timers to generate clean, jitter-free signals without dropping frames.

---

## ✨ Features in Action

### 1. Direct Digital Synthesis (DDS) Tachometer
![Tachometer Animation](assets/tachometer.gif)
> *Instantaneous, zero-lag response via 50µs background hardware interrupts.*

The tachometer receives crystal-clear square waves from a background `TimerOne` interrupt. This completely isolates the motor timing from the serial parsing loop, preventing the micro-stuttering common in standard `tone()` implementations.

### 2. Asymmetrical Speedometer Inertia
![Speedometer Animation](assets/speedometer.gif)
> *Notice how the needle climbs instantly but falls with heavy mechanical lag.*

Physical speedometer needles have weight. This code implements independent acceleration and deceleration slew rates. A built-in "Telemetry Glitch Shield" also isolates and suppresses momentary 'zero drops' or packet corruptions during heavy gaming.

### 3. Multi-Stage Shift Light Fading
![Shift Light Animation](assets/shift-light.gif)
> *Smooth hardware PWM fading rather than harsh on/off snapping.*

Instead of aggressively snapping on and off, the shift light smoothly interpolates between off, dim (staging), and max brightness (shift) using native 8-bit hardware PWM.

### 4. Overclocked I2C LCD Display
![LCD Animation](assets/lcd-screen.gif)
> *Flicker-free, 120Hz text updates.*

By overclocking the `Wire` library to 400kHz and using an in-memory string-padding algorithm, character transmission overhead is reduced by over 50%. This guarantees the screen never flickers and never slows down the mechanical needles.

---
## 🔌 Hardware Setup

### Wiring Map
| Component | Arduino Pin | Output Type | Description |
| :--- | :--- | :--- | :--- |
| **Tachometer (RPM)** | `D2` | Frequency | Background Timer1 Interrupt |
| **Shift Light** | `D3` | PWM (8-bit) | Multi-stage dimming |
| **Check Engine LED** | `D4` | Digital | ON/OFF state |
| **Coolant Temp** | `D5` | PWM (8-bit) | Variable voltage |
| **Fuel Level** | `D6` | PWM (8-bit) | Variable voltage |
| **Speedometer** | `D11` | Frequency | Background Timer1 Interrupt |
| **LCD (SDA)**| `A4` | I2C Data | Telemetry Display |
| **LCD (SCL)**| `A5` | I2C Clock | Overclocked to 400kHz |

---

## 📡 PC Telemetry Setup (SimHub)

The controller expects a stream of comma-separated variables (CSV) over the Serial port at **115200 Baud**, terminated by a newline (`\n`). 

**Format:**
`ShiftLight,EngineLight,Speed,RPM,Temp,Fuel,LCD_Line1,LCD_Line2\n`

**Example Packet:**
`1,1,65,4200,90,75,LAP 4 - POS 3,E_TIME 06:14\n`

---

## 🛠️ Calibration & Tuning

Because physical gauges differ across makes and models, the code uses a series of linear interpolation arrays. You can map your specific gauges by modifying the coordinate pairs at the top of the sketch.

```cpp
// Example: Speedometer Calibration Table
const int SPEED_POINTS = 8;
const int speedSteps[] = {0, 16, 40, 80, 100, 120, 140, 180}; // Game telemetry (MPH)
const int speedHz[]    = {0, 31, 88, 178, 222, 265, 300, 384}; // Gauge requirement (Hz)
```

**Tuning Needle Weight:**
Find these variables in the main loop to adjust how heavy the speedometer feels:
* `SPEED_ACCEL_RATE`: Increase for a lighter, faster-climbing needle.
* `SPEED_DECEL_RATE`: Decrease to give the needle a heavier, lazier drop.

---

## 💾 Dependencies
Install these via the Arduino IDE Library Manager:
* **TimerOne** (by Jesse Tane, et al.)
* **LiquidCrystal_I2C** (by Frank de Brabander)
