---

publishDate: 2026-08-25T00:00:00Z

title: IntelliMap – Portable Indoor Environmental Assessment and Mapping System

excerpt: IntelliMap is a portable ESP32-based system for assessing indoor environmental conditions across different rooms and building levels. It combines temperature, pressure, ambient-light and motion sensing with room labeling, comfort scoring, anomaly detection, Blynk IoT, Bluetooth Low Energy and offline data storage.

image: intellimap-cover.jpg

tags:

* iot
* esp32
* energy-efficiency
* hvac
* indoor-environment
* environmental-monitoring
* blynk
* bluetooth

---

> A portable IoT system that transforms room-by-room environmental measurements into actionable building insights.

---

## Acknowledgements

We acknowledge the MYOSA project and its contributors for providing the hardware platform and development environment that supported the development of IntelliMap.

We also acknowledge the guidance and support received during the development, integration and testing of the hardware, embedded software and IoT dashboard.

---

## Overview

IntelliMap is a **portable indoor environmental assessment and mapping system** developed using an ESP32 microcontroller and multiple sensing and communication technologies.

The system is designed to help users collect and compare environmental conditions across different areas of a building. Instead of depending on a fixed sensor installed at one location, IntelliMap can be physically moved from room to room while automatically detecting when the device has stopped moving.

Once the device is stationary, it records an environmental measurement and associates the reading with a selected room or area.

The system currently measures:

* Temperature
* Atmospheric pressure
* Relative floor level
* Ambient light
* Red, green and blue light channels
* Motion/stationary status
* A calculated 0–100 comfort score

IntelliMap can transmit measurements through **Wi-Fi using Blynk IoT** or through **Bluetooth Low Energy (BLE)**. Measurements are also buffered locally, allowing the system to continue operating when an Internet connection is unavailable.

The system compares new measurements with previously recorded building measurements and identifies significant temperature or light deviations as potential environmental anomalies.

The resulting information can support **building energy assessments, indoor environmental investigations and HVAC performance assessment** by helping identify areas with unusual environmental conditions.

### Key features:

* Portable room-to-room environmental assessment
* Automatic stationary detection before measurement
* Temperature measurement
* Atmospheric pressure measurement
* Relative floor-level estimation
* Ambient-light measurement
* RGB light-channel monitoring
* Gesture-based room selection
* 0–100 comfort score
* Environmental anomaly detection
* Local measurement buffering
* Blynk IoT dashboard
* Bluetooth Low Energy communication
* Offline-first operation
* Automatic synchronization after reconnection
* Wi-Fi network scanning
* OLED-based user interface
* Manual measurement control
* Up to 20 locally buffered room measurements

---

## Demo / Examples

### Images

<p align="center">

<img src="intellimap(1)/intellimap-cover.jpg" width="800"><br/>

<i>IntelliMap portable indoor environmental assessment system.</i>

</p>

<p align="center">

<img src="/assets/images/intellimap/intellimap-hardware.jpg" width="800"><br/>

<i>ESP32, sensors and OLED display used in the IntelliMap prototype.</i>

</p>

<p align="center">

<img src="/assets/images/intellimap/intellimap-oled.jpg" width="800"><br/>

<i>IntelliMap OLED interface displaying system and environmental information.</i>

</p>

<p align="center">

<img src="/assets/images/intellimap/intellimap-blynk.jpg" width="800"><br/>

<i>Blynk IoT dashboard displaying IntelliMap data.</i>

</p>

<p align="center">

<img src="/assets/images/intellimap/intellimap-testing.jpg" width="800"><br/>

<i>IntelliMap during indoor environmental data collection.</i>

</p>

### Videos

The demonstration video should be uploaded as a local MP4 file.

<video controls width="100%">

  <source src="/intellimap-demo.mp4" type="video/mp4">

</video>

---

## Features (Detailed)

### 1. Motion-Aware Measurement

IntelliMap uses the **MPU6050 accelerometer and gyroscope** to determine whether the device is moving or stationary.

The accelerometer continuously samples acceleration magnitude and stores recent measurements in a rolling buffer. The firmware calculates the variance of these measurements to determine whether the device is moving.

When the device becomes stationary, the system starts a confirmation period of approximately **2.5 seconds**.

Only after the user remains still for the required period is the environmental measurement recorded.

This reduces the possibility of capturing measurements while the device is being moved between locations.

The process is:

```plaintext
Moving
   ↓
Motion detected
   ↓
Device becomes stationary
   ↓
Stationary confirmation
   ↓
Measurement recorded
```

---

### 2. Environmental Sensing

When a measurement is triggered, IntelliMap collects data from the BMP180 and APDS9960 sensors.

The **BMP180** measures:

* Temperature
* Atmospheric pressure

The pressure measurement is also used to estimate the device's relative floor level.

The **APDS9960** measures:

* Ambient light
* Clear-channel light
* Red light channel
* Green light channel
* Blue light channel

The APDS9960 is also used for gesture and proximity-based interaction.

The current implementation does **not** measure humidity or air quality.

---

### 3. Relative Floor-Level Estimation

IntelliMap uses pressure-derived altitude to estimate the relative floor level of the building.

At startup, the system captures a baseline altitude. A floor-to-floor height of approximately **3 metres** is used to estimate the relative floor.

This allows measurements to be associated with different building levels.

For example:

```plaintext
Building level
     ↓
Reference level
     ↓
Floor 1
     ↓
Floor 2
     ↓
Floor 3
```

The estimated floor level is stored together with each room measurement.

---

### 4. Gesture-Based Room Selection

The APDS9960 provides a physical gesture interface that allows the user to interact with IntelliMap without relying entirely on the phone dashboard.

The implemented gestures are:

| Gesture        | Function             |
| -------------- | -------------------- |
| Swipe UP       | Select previous room |
| Swipe DOWN     | Select next room     |
| Swipe RIGHT    | Confirm room label   |
| Swipe LEFT     | Start Wi-Fi scan     |
| Proximity/NEAR | Force a measurement  |

Room presets include:

```plaintext
Room 1
Room 2
Room 3
Room 4
Room 5
Corridor
Lobby
Office
Classroom
Storage
```

The room label can also be updated through the Blynk interface.

---

### 5. Comfort Score

IntelliMap calculates a **0–100 Building Comfort Score** using temperature and ambient-light conditions.

The temperature calculation uses a target indoor temperature range of approximately **21–26°C**.

The light component considers very high light levels, which may indicate glare or strong light exposure, and very low light levels.

The resulting score provides a simple way to compare environmental conditions between rooms.

---

### 6. Environmental Anomaly Detection

The system compares each new room measurement with the running building average.

The current implementation detects anomalies based on:

* Temperature deviation
* Ambient-light deviation

A temperature reading that differs from the running average by more than approximately **3.5°C** is flagged.

A light reading that differs from the running average by more than approximately **2000 raw counts** is also flagged.

When an anomaly is detected, IntelliMap:

* Marks the measurement as anomalous
* Activates the anomaly indicator
* Records the event
* Sends an anomaly event to Blynk when connected

This allows users to identify rooms that may require further investigation.

---

### 7. Blynk IoT Dashboard

IntelliMap uses **Blynk IoT** to provide remote monitoring and control.

The implemented virtual datastreams are:

```plaintext
V0  → Temperature
V1  → Pressure
V2  → Relative floor level
V3  → Ambient light
V4  → Comfort score
V5  → Recording status
V6  → Room label
V7  → Manual measurement button
V8  → Anomaly indicator
V9  → Event log
V10 → Number of rooms recorded
V11 → Number of nearby Wi-Fi networks
V12 → Wi-Fi scan button
V13 → Nearby network log
V14 → BLE connection status
V15 → Red light channel
V16 → Green light channel
V17 → Blue light channel
V18 → ALS validity status
```

The dashboard provides a remote view of the environmental assessment process.

---

### 8. Bluetooth Low Energy

IntelliMap also provides Bluetooth Low Energy communication.

The ESP32 operates as a BLE GATT server and sends environmental measurements as JSON data.

Example:

```json
{
  "room": "Room 1",
  "temp": 24.5,
  "pressure": 1012.4,
  "floor": 1,
  "light": 1250,
  "comfort": 94,
  "anomaly": false
}
```

BLE allows a compatible phone or application to receive measurements without requiring an Internet connection.

---

### 9. Offline-First Operation

IntelliMap is designed so that environmental measurements do not depend on continuous Wi-Fi or Internet connectivity.

When the cloud connection is unavailable, the system continues to perform:

* Sensor measurements
* Motion detection
* Room selection
* Comfort-score calculation
* Anomaly detection
* OLED display
* Local data buffering
* BLE communication

When the Blynk connection is restored, previously unsynchronized measurements are automatically synchronized.

This makes IntelliMap suitable for indoor environments where network connectivity may be unreliable.

---

### 10. Wi-Fi Network Scanning

The system can scan for nearby Wi-Fi networks.

The scan records:

* SSID
* Signal strength
* Security status

Results can be displayed on the OLED, Blynk dashboard and BLE connection.

The scan can be triggered automatically or manually through the APDS9960 swipe-left gesture or the Blynk scan button.

---

### 11. OLED Interface

IntelliMap includes an **SSD1306 128×64 OLED display**.

The display provides:

* Startup information
* Sensor initialization status
* Current room
* Motion status
* Stationary countdown
* Environmental measurement results
* Comfort score
* Anomaly notification
* Wi-Fi scan results
* Network status

This allows the device to operate as a standalone portable assessment tool.

---

### 12. Local Data Buffer

Each measurement is stored in a `RoomReading` structure containing:

* Room label
* Temperature
* Pressure
* Floor level
* Clear-channel light
* Red light channel
* Green light channel
* Blue light channel
* ALS validity
* Comfort score
* Anomaly status
* Synchronization status

The current implementation can store up to **20 measurements** locally before the oldest measurement is replaced.

---

## Usage Instructions

### 1. Assemble the Hardware

Connect the ESP32, sensors and OLED display.

The devices share an I2C bus:

```plaintext
SDA → GPIO 21
SCL → GPIO 22
VCC → 3.3V
GND → GND
```

The system uses:

```plaintext
MPU6050  → Motion/stationary detection
BMP180   → Temperature + pressure
APDS9960 → Light + gesture + proximity
SSD1306  → OLED interface
```

Dedicated interrupt lines:

```plaintext
MPU6050 INT  → GPIO 34
APDS9960 INT → GPIO 35
```

The current firmware polls the sensors, so the interrupt lines are available for future improvements.

### 2. Install the Software

Install Arduino IDE and the ESP32 board package.

Install the required libraries listed in the **Requirements / Installation** section.

### 3. Configure Wi-Fi and Blynk

Enter the required Wi-Fi and Blynk configuration in the firmware.

For the public GitHub version, use placeholders rather than real credentials.

### 4. Upload the Firmware

Connect the ESP32 to a computer using USB.

Open the IntelliMap `.ino` file in Arduino IDE.

Select the correct ESP32 board and COM port.

Upload the firmware.

### 5. Start the Device

Power IntelliMap.

The OLED displays the startup process while the sensors, BLE and Wi-Fi services initialize.

### 6. Select a Room

Use the APDS9960 gesture interface to select the location.

Swipe UP or DOWN to change the room preset.

Swipe RIGHT to confirm the room.

### 7. Collect a Measurement

Move to the desired location and stop.

The MPU6050 detects when the device becomes stationary.

Remain still during the approximately 2.5-second confirmation period.

The system then records the environmental conditions.

### 8. Monitor the Results

The measurement appears on the OLED.

If connected to Blynk, the information is also transmitted to the dashboard.

BLE-connected devices can also receive the measurement.

### 9. Repeat

Move to the next location and repeat the measurement process.

Measurements can then be compared across different rooms and building levels.

### 10. Analyze the Building

Use the collected measurements to identify environmental differences and potential areas requiring further investigation during an energy or HVAC assessment.

---

## Tech Stack

### Hardware

* **ESP32 Dev Board** – Main processing and communication
* **MPU6050** – Accelerometer and gyroscope
* **BMP180** – Temperature and pressure sensor
* **APDS9960** – Ambient-light, gesture and proximity sensor
* **SSD1306 OLED 128×64** – Local display

### Software

* **Arduino IDE**
* **C/C++**
* **Blynk IoT**
* **ESP32 BLE Arduino**

### Communication

* **I2C**
* **Wi-Fi**
* **Bluetooth Low Energy**

### Libraries

* Adafruit MPU6050
* Adafruit Unified Sensor
* Adafruit BMP085
* Adafruit APDS9960
* Adafruit GFX
* Adafruit SSD1306
* Blynk
* ESP32 BLE Arduino
* WiFi
* WiFiMulti
* Wire

---

## Requirements / Installation

### Hardware

```plaintext
1 × ESP32 development board
1 × MPU6050
1 × BMP180
1 × APDS9960
1 × SSD1306 128×64 OLED
Jumper wires
Breadboard or PCB
USB cable
Power source
```

### Software

```plaintext
Arduino IDE
ESP32 board package
Blynk IoT account
Required Arduino libraries
```

Install the required libraries through Arduino IDE's Library Manager:

```plaintext
Adafruit MPU6050
Adafruit Unified Sensor
Adafruit BMP085
Adafruit APDS9960
Adafruit SSD1306
Adafruit GFX
Blynk
```

The ESP32 BLE library is included with the ESP32 board package.

### Configuration

```cpp
#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "IntelliMap"
#define BLYNK_AUTH_TOKEN    "YOUR_AUTH_TOKEN"
```

Configure Wi-Fi:

```cpp
const char* WIFI_SSIDS[4] = {
  "YOUR_WIFI_1",
  "YOUR_WIFI_2",
  "YOUR_WIFI_3",
  "YOUR_WIFI_4"
};

const char* WIFI_PASSWORDS[4] = {
  "YOUR_PASSWORD_1",
  "YOUR_PASSWORD_2",
  "YOUR_PASSWORD_3",
  "YOUR_PASSWORD_4"
};
```

Open the Serial Monitor at:

```plaintext
115200 baud
```

---

## File Structure

```plaintext
/intellimap
│
├── intellimap.md
├── intellimap-cover.jpg
├── intellimap-hardware.jpg
├── intellimap-oled.jpg
├── intellimap-blynk.jpg
├── intellimap-testing.jpg
├── intellimap-demo.mp4
│
└── src/
    └── intellimap.ino
```

---

## License

This project is intended to follow the open-source requirements and licensing guidelines of the MYOSA project repository.

Third-party libraries remain subject to their respective licenses.

---

## Contribution Notes

Future improvements may include:

* More accurate indoor positioning
* Automatic generation of indoor environmental maps
* Improved floor-level estimation
* Additional environmental sensors
* Improved light calibration
* Historical data visualization
* Mobile companion application
* Advanced anomaly detection
* Machine-learning-based environmental analysis
* Integration with building management systems
* Automated HVAC optimization recommendations
* Larger-scale building deployment
