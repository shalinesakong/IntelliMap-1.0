/*
  ============================================================================
  IntelliMap — Portable Indoor Environmental Assessment System
  ============================================================================

  HARDWARE:
    - ESP32 Dev Board
    - MPU6050  (Accelerometer + Gyroscope) -> motion / stationary detection
    - BMP180   (Barometric Pressure + Temperature) -> temp + relative floor level
    - APDS9960 (Gesture + Proximity + Ambient Light) -> lighting + gesture UI
    - SSD1306 OLED 128x64 (I2C) -> on-device UI

  WIRING (single shared I2C bus — all four devices on the same two pins):
    SDA -> GPIO 21
    SCL -> GPIO 22
    VCC -> 3.3V
    GND -> GND

    I2C is a shared-bus protocol: every device is told apart by its own
    address, not a dedicated wire, so all four sensors sit in parallel on
    the same SDA/SCL pair. Their addresses don't collide, so this is safe:
      - OLED (SSD1306)  addr 0x3C
      - BMP180          addr 0x77
      - MPU6050         addr 0x68
      - APDS9960        addr 0x39

    DEDICATED INTERRUPT PINS (genuinely unique per sensor, not shared —
    these are separate from the I2C data lines above):
      MPU6050 INT  -> GPIO 34 (input-only pin; wired, available for
                                future motion-interrupt wake-up)
      APDS9960 INT -> GPIO 35 (input-only pin; wired, available for
                                future gesture-ready interrupt)
    Neither interrupt pin is required for this sketch to work — both
    sensors are polled — but they're wired and ready if you later want
    to switch to interrupt-driven, lower-power operation.

  REQUIRED LIBRARIES (Arduino Library Manager):
    - Adafruit MPU6050
    - Adafruit Unified Sensor
    - Adafruit BMP085 Library      (works for BMP180)
    - Adafruit APDS9960 Library
    - Adafruit SSD1306
    - Adafruit GFX Library
    - Blynk (Blynk IoT, v1.x)      by Volodymyr Shymanskyy
    - ESP32 BLE Arduino            (bundled with the ESP32 board package —
                                     do NOT also install a standalone copy
                                     of this library, it will conflict)

  WHAT THIS SKETCH DOES (beyond basic sensor printing):
    1. Runs a non-blocking state machine (IDLE -> CONFIRM_STATIONARY ->
       RECORDING -> RESULT) driven by MPU6050 motion variance, so a
       measurement is only ever taken when the user has genuinely stopped
       walking — exactly as described in the proposal.
    2. Reads ambient light straight off the APDS9960's raw ALS data
       registers (same technique as APDS9960_ALS_RawDiagnostic.ino) rather
       than through the Adafruit library's color-data helpers, and
       publishes the Clear/R/G/B channels + AVALID status to both the OLED
       and the Blynk dashboard every time a measurement is taken.
    3. Uses APDS9960 gestures as a physical UI: swipe UP/DOWN to cycle
       through preset room labels, swipe RIGHT to confirm/save a room,
       swipe LEFT triggers a Wi-Fi network scan, a hand held NEAR
       triggers a manual force-measurement.
    4. Converts BMP180 pressure into a *relative* floor level using a
       floor baseline captured at startup (per ~3m floor-to-floor height).
    5. Computes a 0-100 "Building Comfort Score" from temperature and
       light exposure, and buffers each room's data in a struct array.
    6. Detects environmental anomalies by comparing each new room reading
       against the running building average (temp spike, light spike,
       floor-to-floor imbalance) and raises a Blynk event/notification.
    7. Streams everything live to a Blynk IoT dashboard over Wi-Fi (gauges
       for temp/pressure/light/comfort score, a floor indicator, a
       room-label text box, an anomaly LED, and a datastream log).
    8. ALSO broadcasts every reading over Bluetooth Low Energy as a JSON
       payload on a custom GATT characteristic, with notify support, so a
       phone/companion app can receive live data even without Wi-Fi —
       the two transports run simultaneously (ESP32 handles Wi-Fi + BLE
       coexistence in the SDK).
    9. Runs a non-blocking asynchronous Wi-Fi network scan on a timer
       (and on-demand via gesture or Blynk button), listing every SSID
       in range with signal strength, and reports the list to both the
       OLED and the Blynk dashboard.
   10. Drives a multi-screen OLED UI (splash, live readings, "hold still"
       countdown, result screen, and a temporary Wi-Fi scan-results
       overlay) instead of a single static screen.
   11. OFFLINE-FIRST DESIGN: nothing in this sketch blocks waiting for
       Wi-Fi or the Blynk cloud. Motion detection, room recording, gesture
       control, the OLED UI, local buffering, and BLE broadcasting all
       keep working with zero network in range. Wi-Fi/Blynk connect in
       the background on their own retry timers; whenever the cloud link
       comes up, any readings captured while offline are automatically
       synced.
   12. ALWAYS-ON Wi-Fi: once a connection succeeds, modem-sleep is
       disabled (WiFi.setSleep(false)) and auto-reconnect is enabled
       (WiFi.setAutoReconnect(true)), so the radio stays fully awake and
       reattaches immediately on its own if the link drops, on top of the
       existing wifiMulti retry loop in maintainConnectivity().
  ============================================================================
*/


// ------------------------- BLYNK CONFIG (edit these) ----------------------
#define BLYNK_TEMPLATE_ID   "TMPL2nop5Z4dH"      // <-- from Blynk.Console
#define BLYNK_TEMPLATE_NAME "IntelliMap"
#define BLYNK_AUTH_TOKEN    "PcOz2lDAYXQEHOo6u70up5o7d_cF8IZZ"

// ------------------------- WI-FI CONFIG (edit these) -----------------------
// Add up to 4 networks — the device connects to whichever is in range,
// preferring the strongest signal, and switches automatically if the
// current one drops and another becomes available. Leave unused slots
// as "" to disable them.
const char* WIFI_SSIDS[4]     = { "Viva",        "Shee😍",   "DESKTOP-JU5ED4V 6299", "Galaxy A3 Core2451" };
const char* WIFI_PASSWORDS[4] = { "Sakong@8439", "123456789", "Shee2020",            "hkbv0670" };

// ------------------------------ INCLUDES -----------------------------------
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_APDS9960.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <BlynkSimpleEsp32.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ------------------------------ I2C BUS / PIN CONFIG ------------------------
#define I2C_SDA 21   // shared by OLED, BMP180, MPU6050, APDS9960
#define I2C_SCL 22

#define MPU_INT_PIN  34  // dedicated interrupt line, MPU6050 only
#define APDS_INT_PIN 35  // dedicated interrupt line, APDS9960 only

// ------------------------------ OLED SETUP ---------------------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
//Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ------------------------------ SENSOR OBJECTS ------------------------------
Adafruit_MPU6050 mpu;
Adafruit_BMP085 bmp;
Adafruit_APDS9960 apds;

WiFiMulti wifiMulti; // manages the up-to-4 configured networks above

// ------------------------- APDS9960 RAW ALS REGISTERS -----------------------
// Ambient light is read directly off the chip's data registers (same
// technique as APDS9960_ALS_RawDiagnostic.ino) instead of through the
// Adafruit library's getColorData()/colorDataReady() polling helpers. This
// gives us the actual AVALID flag plus all four raw channel counts
// (Clear/R/G/B) straight from the sensor. Gesture/proximity still go
// through the Adafruit library as before — only the ambient-light path
// is raw.
#define APDS_REG_STATUS  0x93
#define APDS_REG_CDATAL  0x94 // clear low byte; C/R/G/B each 2 bytes, sequential

uint8_t apdsReadReg(uint8_t reg) {
  Wire.beginTransmission(APDS9960_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)APDS9960_ADDRESS, 1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

uint16_t apdsRead16(uint8_t lowReg) {
  Wire.beginTransmission(APDS9960_ADDRESS);
  Wire.write(lowReg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)APDS9960_ADDRESS, 2);
  uint16_t lo = Wire.available() ? Wire.read() : 0;
  uint16_t hi = Wire.available() ? Wire.read() : 0;
  return (hi << 8) | lo;
}

// Reads the four raw ALS channels straight from the data registers and
// returns the AVALID bit (true = fresh reading, false = stale/not-yet-ready).
bool readRawALS(uint16_t &c, uint16_t &r, uint16_t &g, uint16_t &b) {
  uint8_t status = apdsReadReg(APDS_REG_STATUS);
  bool avalid = status & 0x01;

  c = apdsRead16(APDS_REG_CDATAL);
  r = apdsRead16(APDS_REG_CDATAL + 2);
  g = apdsRead16(APDS_REG_CDATAL + 4);
  b = apdsRead16(APDS_REG_CDATAL + 6);

  return avalid;
}

// --------------------------- BLYNK VIRTUAL PINS -----------------------------
#define V_TEMP        V0   // Gauge: Temperature (C)
#define V_PRESSURE    V1   // Gauge: Pressure (hPa)
#define V_FLOOR       V2   // Value: Relative floor level
#define V_LIGHT       V3   // Gauge: Ambient light (raw APDS9960 clear channel)
#define V_COMFORT     V4   // Gauge: Comfort score 0-100
#define V_STATUS_LED  V5   // LED: lit while recording
#define V_ROOM_LABEL  V6   // Label / text display: current room name
#define V_MANUAL_BTN  V7   // Button: force a manual measurement
#define V_ANOMALY_LED V8   // LED: lit red when an anomaly is flagged
#define V_LOG         V9   // Terminal widget: human-readable event log
#define V_ROOM_COUNT  V10  // Value: number of rooms recorded so far
#define V_NET_COUNT   V11  // Value: number of Wi-Fi networks found nearby
#define V_SCAN_BTN    V12  // Button: force a manual Wi-Fi scan
#define V_NET_LOG     V13  // Terminal widget: nearby network list (SSID/RSSI)
#define V_BLE_STATUS  V14  // LED: lit while a BLE client is connected
#define V_LIGHT_R     V15  // Gauge: raw ALS Red channel
#define V_LIGHT_G     V16  // Gauge: raw ALS Green channel
#define V_LIGHT_B     V17  // Gauge: raw ALS Blue channel
#define V_ALS_VALID   V18  // LED: lit while the last ALS reading was AVALID (fresh)

// --------------------------- BLE CONFIG -------------------------------------
// Any random valid UUIDs work here — just keep the phone/companion app in sync.
#define BLE_SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_DATA_CHAR_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // notify: sensor JSON
#define BLE_NETWORK_CHAR_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // notify: scan JSON

BLEServer* pServer = nullptr;
BLECharacteristic* pDataCharacteristic = nullptr;
BLECharacteristic* pNetworkCharacteristic = nullptr;
bool bleClientConnected = false;

class BLEConnectionCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    bleClientConnected = true;
  }
  void onDisconnect(BLEServer* server) override {
    bleClientConnected = false;
    server->getAdvertising()->start(); // resume advertising so a phone can reconnect
  }
};

// ------------------------------ TUNABLE CONSTANTS ---------------------------
const float MOTION_VARIANCE_THRESHOLD = 0.35f;   // accel variance => "moving"
const unsigned long STATIONARY_HOLD_MS = 2500;    // must be still this long
const unsigned long SAMPLE_INTERVAL_MS = 100;     // motion sampling rate
const unsigned long DISPLAY_RESULT_MS  = 30000;   // how long the readings/parameters screen shows
const float FLOOR_HEIGHT_M = 3.0f;                // assumed floor-to-floor height
const float SEA_LEVEL_HPA  = 1013.25f;            // standard pressure reference

const int   COMFORT_IDEAL_TEMP_LOW  = 21;
const int   COMFORT_IDEAL_TEMP_HIGH = 26;
const int   MAX_ROOMS = 20;                       // buffer capacity

const unsigned long WIFI_SCAN_INTERVAL_MS = 30000; // auto re-scan every 30s
const unsigned long WIFI_SCAN_DISPLAY_MS  = 20000;  // how long other screen sections show on OLED
const int   MAX_NETWORKS_SHOWN = 4;                // OLED can't fit more

const char* ROOM_PRESETS[] = {
  "Room 1", "Room 2", "Room 3", "Room 4", "Room 5",
  "Corridor", "Lobby", "Office", "Classroom", "Storage"
};
const int NUM_PRESETS = sizeof(ROOM_PRESETS) / sizeof(ROOM_PRESETS[0]);

// ------------------------------ DATA STRUCTURES ------------------------------
struct RoomReading {
  char  label[20];
  float temperature;
  float pressure;
  int   floorLevel;
  uint16_t light;    // raw ALS clear channel
  uint16_t lightR;    // raw ALS red channel
  uint16_t lightG;    // raw ALS green channel
  uint16_t lightB;    // raw ALS blue channel
  bool  alsValid;     // AVALID bit at time of read (fresh vs stale)
  int   comfortScore;
  bool  anomaly;
  bool  synced; // true once successfully pushed to the Blynk cloud
};

RoomReading rooms[MAX_ROOMS];
int roomCount = 0;
int presetIndex = 0;

// ------------------------------ STATE MACHINE --------------------------------
enum SystemState {
  STATE_IDLE,               // watching for motion to stop
  STATE_CONFIRM_STATIONARY, // counting down while still
  STATE_RECORDING,          // taking the actual measurement
  STATE_RESULT              // showing the result briefly
};
SystemState currentState = STATE_IDLE;

unsigned long lastSampleTime   = 0;
unsigned long stationarySince  = 0;
unsigned long resultShownAt    = 0;

// rolling accel buffer for variance calc
const int ACCEL_WINDOW = 10;
float accelMagBuffer[ACCEL_WINDOW];
int   accelBufIndex = 0;
bool  accelBufFilled = false;

float baselineAltitude = 0.0f;
bool  baselineCaptured = false;

// Wi-Fi scanning state
unsigned long lastWifiScanTime   = 0;
unsigned long wifiScanShownAt    = 0;
bool wifiScanInProgress          = false;
bool wifiScanPending             = false; // scan finished, waiting for a safe moment to show
bool showWifiScanScreen          = false;
int  lastNetworkCount            = 0;
String lastNetworkSummary        = ""; // top networks, "SSID (-RSSIdBm)\n..."

// Connectivity state — everything else in this sketch works with none of
// this ever becoming true. These just track background connection attempts.
bool wifiConnected               = false;
bool wasBlynkConnected           = false;
unsigned long lastWifiAttempt    = 0;
unsigned long lastBlynkAttempt   = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS  = 20000; // retry joining Wi-Fi
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 5000;  // bounded attempt length, tries all 4 APs
const unsigned long BLYNK_RETRY_INTERVAL_MS = 15000; // retry cloud connect
const unsigned long BLYNK_CONNECT_TIMEOUT_MS = 1500; // bounded attempt length

// ------------------------------ FORWARD DECLARATIONS --------------------------
void updateMotionBuffer();
float computeAccelVariance();
void runStateMachine();
void takeMeasurement();
bool readRawALS(uint16_t &c, uint16_t &r, uint16_t &g, uint16_t &b);
void publishRawALS(RoomReading &r);
int  computeComfortScore(float tempC, uint16_t light);
bool detectAnomaly(RoomReading &r);
void handleGesture();
void drawIdleScreen(float variance);
void drawCountdownScreen(unsigned long remainingMs);
void drawResultScreen(RoomReading &r);
void drawWifiScanScreen();
bool pushToBlynk(RoomReading &r); // returns true only if actually sent
void logEvent(const String &msg);
void setupBLE();
void bleBroadcastReading(RoomReading &r);
void startWifiScan();
void checkWifiScan();
void maintainConnectivity();
void syncPendingReadings();

// ==============================================================================
void setup() {
  Serial.begin(115200);

  // Single shared I2C bus — OLED, BMP180, MPU6050, and APDS9960 all sit on
  // this one SDA/SCL pair, distinguished by their own I2C addresses.
  Wire.begin(I2C_SDA, I2C_SCL);

  // Dedicated per-sensor interrupt pins (not yet used for edge-triggered
  // wake in this sketch, but wired and ready for that enhancement)
  pinMode(MPU_INT_PIN, INPUT);
  pinMode(APDS_INT_PIN, INPUT);

  // ---- OLED init ----
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 init failed"));
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("IntelliMap"));
  display.println(F("Booting sensors..."));
  display.display();

  // ---- MPU6050 init ----
  // Some breakout boards pull AD0 high, changing the I2C address from the
  // default 0x68 to 0x69 (confirmed on this board via I2C scanner). Try
  // the default first, then fall back to 0x69 automatically.
  if (!mpu.begin(0x68, &Wire) && !mpu.begin(0x69, &Wire)) {
    Serial.println(F("MPU6050 not found"));
    display.println(F("MPU6050 FAIL"));
    display.display();
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  // ---- BMP180 init ----
  if (!bmp.begin()) {
    Serial.println(F("BMP180 not found"));
    display.println(F("BMP180 FAIL"));
    display.display();
  } else {
   // Shift baseline down by one floor so current location reads as floor 1
   baselineAltitude = bmp.readAltitude(SEA_LEVEL_HPA * 100.0f) - FLOOR_HEIGHT_M;
   baselineCaptured = true;

  }

  // ---- APDS9960 init ----
  // NOTE: if this reports "not found" even though an I2C scanner sees 0x39,
  // your board is a clone reporting a different chip ID than Adafruit
  // expects. The Adafruit_APDS9960 library hard-codes a check for ID 0xAB
  // in begin() (Adafruit_APDS9960.cpp, ~line 100). Many clones report 0xA8.
  // Fix: open that file (find it under your sketchbook's libraries folder,
  // e.g. Documents\Arduino\libraries\Adafruit_APDS9960\Adafruit_APDS9960.cpp)
  // and change:
  //     if (x != 0xAB) { return false; }
  // to:
  //     if (x != 0xAB && x != 0xA8) { return false; }
  // Save, then re-upload this sketch.
  if (!apds.begin(10, APDS9960_AGAIN_4X, APDS9960_ADDRESS, &Wire)) {
    Serial.println(F("APDS9960 not found"));
    display.println(F("APDS9960 FAIL"));
    display.display();
  } else {
    apds.enableProximity(true);
    apds.enableGesture(true);
    apds.enableColor(true);
    apds.setADCGain(APDS9960_AGAIN_16X); // 4x was too dim for typical indoor
                                          // lighting — 16x gives much better
                                          // sensitivity to a room lamp/bulb
  }

  // ---- Wi-Fi / Blynk (fully non-blocking, up to 4 networks) ----
  // Registering networks with WiFiMulti and calling Blynk.config() both
  // return immediately — they do NOT wait for a connection. Everything
  // below this point (sensors, BLE, gesture UI, state machine) runs
  // identically whether or not any network is ever available. Actual
  // connecting happens in the background via maintainConnectivity(),
  // called every loop() — it tries whichever configured network has the
  // strongest signal in range, and fails over automatically if that one
  // drops later.
  display.println(F("Wi-Fi connecting (background)"));
  display.display();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // disable modem-sleep from the start so the radio
                         // never idles down, even during the initial connect
  for (uint8_t i = 0; i < 4; i++) {
    if (strlen(WIFI_SSIDS[i]) > 0) {
      wifiMulti.addAP(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);
    }
  }
  Blynk.config(BLYNK_AUTH_TOKEN);

  // ---- Bluetooth Low Energy (runs alongside Wi-Fi) ----
  setupBLE();

  for (int i = 0; i < ACCEL_WINDOW; i++) accelMagBuffer[i] = 0;

  strcpy(rooms[0].label, ROOM_PRESETS[0]);

  delay(800);
  logEvent("System ready. Wi-Fi + BLE data sharing active.");

  startWifiScan(); // populate the nearby-network list right away
}

// ==============================================================================
void loop() {
  maintainConnectivity(); // non-blocking Wi-Fi + Blynk housekeeping
  handleGesture();
  checkWifiScan();

  // Periodic background re-scan of nearby Wi-Fi networks — only while we
  // don't yet have a connection. Once wifiMulti has joined a network, this
  // stops automatically (no point burning radio time re-scanning); it
  // resumes on its own if the connection ever drops. A manual scan via
  // gesture-left or the Blynk button still works at any time regardless.
  if (!wifiConnected && !wifiScanInProgress &&
      millis() - lastWifiScanTime > WIFI_SCAN_INTERVAL_MS) {
    startWifiScan();
  }

  runStateMachine(); // sensors, gestures, OLED, recording — all offline-safe
}

// ------------------------------------------------------------------------------
// Samples MPU6050 accel magnitude into a rolling buffer used for variance calc
// ------------------------------------------------------------------------------
void updateMotionBuffer() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float mag = sqrt(a.acceleration.x * a.acceleration.x +
                    a.acceleration.y * a.acceleration.y +
                    a.acceleration.z * a.acceleration.z);

  accelMagBuffer[accelBufIndex] = mag;
  accelBufIndex = (accelBufIndex + 1) % ACCEL_WINDOW;
  if (accelBufIndex == 0) accelBufFilled = true;
}

// ------------------------------------------------------------------------------
// Variance of the buffered accel magnitudes -> proxy for "am I walking?"
// ------------------------------------------------------------------------------
float computeAccelVariance() {
  int n = accelBufFilled ? ACCEL_WINDOW : accelBufIndex;
  if (n < 2) return 999.0f; // not enough data yet, assume moving

  float mean = 0;
  for (int i = 0; i < n; i++) mean += accelMagBuffer[i];
  mean /= n;

  float variance = 0;
  for (int i = 0; i < n; i++) {
    float d = accelMagBuffer[i] - mean;
    variance += d * d;
  }
  variance /= n;
  return variance;
}

// ------------------------------------------------------------------------------
// Core non-blocking state machine
// ------------------------------------------------------------------------------
void runStateMachine() {
  unsigned long now = millis();

  if (now - lastSampleTime < SAMPLE_INTERVAL_MS) return;
  lastSampleTime = now;

  updateMotionBuffer();
  float variance = computeAccelVariance();
  bool isStill = (variance < MOTION_VARIANCE_THRESHOLD);

  // A finished Wi-Fi scan only takes over the OLED once we're back at the
  // idle screen — it never interrupts an in-progress "hold still" countdown
  // or a just-recorded result, so you always get the full 4 seconds to
  // actually read a room's measurement before anything else appears.
  if (wifiScanPending && currentState == STATE_IDLE) {
    wifiScanPending = false;
    showWifiScanScreen = true;
    wifiScanShownAt = now;
  }

  if (showWifiScanScreen) {
    drawWifiScanScreen();
    if (now - wifiScanShownAt >= WIFI_SCAN_DISPLAY_MS) {
      showWifiScanScreen = false;
    }
    return;
  }

  switch (currentState) {

    case STATE_IDLE:
      drawIdleScreen(variance);
      if (Blynk.connected()) Blynk.virtualWrite(V_STATUS_LED, 0);
      if (isStill) {
        stationarySince = now;
        currentState = STATE_CONFIRM_STATIONARY;
      }
      break;

    case STATE_CONFIRM_STATIONARY: {
      if (!isStill) {
        currentState = STATE_IDLE; // user moved again, abort
        break;
      }
      unsigned long held = now - stationarySince;
      if (held >= STATIONARY_HOLD_MS) {
        currentState = STATE_RECORDING;
      } else {
        drawCountdownScreen(STATIONARY_HOLD_MS - held);
      }
      break;
    }

    case STATE_RECORDING:
      if (Blynk.connected()) Blynk.virtualWrite(V_STATUS_LED, 255);
      takeMeasurement(); // records locally + BLE regardless of connectivity
      resultShownAt = now;
      currentState = STATE_RESULT;
      break;

    case STATE_RESULT:
      if (roomCount > 0) drawResultScreen(rooms[roomCount - 1]);
      if (now - resultShownAt >= DISPLAY_RESULT_MS) {
        if (Blynk.connected()) Blynk.virtualWrite(V_STATUS_LED, 0);
        currentState = STATE_IDLE;
      }
      break;
  }
}

// ------------------------------------------------------------------------------
// Reads all sensors, builds a RoomReading, scores it, checks for anomalies,
// stores it, and pushes it to Blynk.
// ------------------------------------------------------------------------------
void takeMeasurement() {
  if (roomCount >= MAX_ROOMS) {
    logEvent("Buffer full - oldest room will be overwritten.");
    roomCount = MAX_ROOMS - 1;
    // simple ring-buffer shift
    for (int i = 0; i < MAX_ROOMS - 1; i++) rooms[i] = rooms[i + 1];
  }

  RoomReading r;
  strncpy(r.label, ROOM_PRESETS[presetIndex], sizeof(r.label));

  r.temperature = bmp.readTemperature();
  r.pressure    = bmp.readPressure() / 100.0f; // Pa -> hPa

  float altitude = bmp.readAltitude(SEA_LEVEL_HPA * 100.0f);
  float relative = baselineCaptured ? (altitude - baselineAltitude) : 0.0f;
  r.floorLevel = (int) round(relative / FLOOR_HEIGHT_M);

  // Ambient light: read raw off the data registers (same technique as
  // APDS9960_ALS_RawDiagnostic.ino) and immediately publish the Clear/R/G/B
  // channels + AVALID status to the OLED and Blynk dashboard.
  publishRawALS(r);

  r.comfortScore = computeComfortScore(r.temperature, r.light);
  r.anomaly = detectAnomaly(r);
  r.synced = false; // becomes true only if we successfully push it now

  rooms[roomCount] = r;
  roomCount++;

  bleBroadcastReading(rooms[roomCount - 1]); // over Bluetooth LE, always —
                                              // doesn't require Wi-Fi at all
  rooms[roomCount - 1].synced = pushToBlynk(rooms[roomCount - 1]);
  // pushToBlynk() itself checks Blynk.connected() and no-ops if offline;
  // any reading that misses this window gets synced automatically the
  // next time the cloud connection comes up (see syncPendingReadings()).

  String msg = String(r.label) + ": " + String(r.temperature, 1) + "C, " +
               String(r.pressure, 1) + "hPa, floor " + String(r.floorLevel) +
               ", light " + String(r.light) + ", score " + String(r.comfortScore);
  if (r.anomaly) msg += " [ANOMALY]";
  if (!rooms[roomCount - 1].synced) msg += " [offline - queued]";
  logEvent(msg);
}

// ------------------------------------------------------------------------------
// Reads the APDS9960's raw ALS registers (readRawALS(), same technique as
// APDS9960_ALS_RawDiagnostic.ino) and publishes the four channels — the
// exact values that diagnostic sketch prints to the Serial terminal — to
// the OLED and the Blynk dashboard instead. Fills in the ALS fields of the
// RoomReading passed in, so the caller doesn't need a separate read step.
// ------------------------------------------------------------------------------
void publishRawALS(RoomReading &r) {
  uint16_t c = 0, red = 0, green = 0, blue = 0;
  bool avalid = readRawALS(c, red, green, blue);

  if (!avalid) {
    logEvent("Warning: ambient light sensor reports AVALID=0 — reading may be stale.");
  }

  r.light    = c;
  r.lightR   = red;
  r.lightG   = green;
  r.lightB   = blue;
  r.alsValid = avalid;

  // ---- Publish to LCD (OLED) ----
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println(F("APDS9960 Raw ALS"));
  display.print(F("AVALID: ")); display.println(avalid);
  display.print(F("Clear: "));  display.println(c);
  display.print(F("R: "));      display.println(red);
  display.print(F("G: "));      display.println(green);
  display.print(F("B: "));      display.println(blue);
  display.display();
  delay(400); // brief hold so the raw-ALS screen is actually readable before
              // the state machine's next OLED screen draws over it

  // ---- Publish to Blynk ----
  if (Blynk.connected()) {
    Blynk.virtualWrite(V_LIGHT,    c);
    Blynk.virtualWrite(V_LIGHT_R,  red);
    Blynk.virtualWrite(V_LIGHT_G,  green);
    Blynk.virtualWrite(V_LIGHT_B,  blue);
    Blynk.virtualWrite(V_ALS_VALID, avalid ? 255 : 0);
  }
}

// ------------------------------------------------------------------------------
// 0-100 comfort score: penalizes temperature outside the ideal band and
// excessive light exposure (proxy for solar heat gain / glare).
// ------------------------------------------------------------------------------
int computeComfortScore(float tempC, uint16_t light) {
  int score = 100;

  if (tempC < COMFORT_IDEAL_TEMP_LOW) {
    score -= (int)((COMFORT_IDEAL_TEMP_LOW - tempC) * 6);
  } else if (tempC > COMFORT_IDEAL_TEMP_HIGH) {
    score -= (int)((tempC - COMFORT_IDEAL_TEMP_HIGH) * 6);
  }

  // Light: assume 0-4000 raw clear-channel counts is a comfortable indoor
  // range; beyond that, penalize proportionally (direct sun / glare).
  if (light > 4000) {
    score -= (int)((light - 4000) / 100);
  } else if (light < 200) {
    score -= (int)((200 - light) / 10); // too dark
  }

  if (score < 0) score = 0;
  if (score > 100) score = 100;
  return score;
}

// ------------------------------------------------------------------------------
// Flags a reading as anomalous if it deviates sharply from the running
// building average recorded so far (temperature spike, light spike, or a
// floor-to-floor jump inconsistent with neighboring rooms).
// ------------------------------------------------------------------------------
bool detectAnomaly(RoomReading &r) {
  if (roomCount == 0) return false; // first room, nothing to compare to

  float avgTemp = 0, avgLight = 0;
  for (int i = 0; i < roomCount; i++) {
    avgTemp  += rooms[i].temperature;
    avgLight += rooms[i].light;
  }
  avgTemp  /= roomCount;
  avgLight /= roomCount;

  bool tempAnomaly  = fabs(r.temperature - avgTemp) > 3.5f;   // >3.5C off avg
  bool lightAnomaly = fabs((float)r.light - avgLight) > 2000; // strong glare/dark

  if (tempAnomaly || lightAnomaly) {
    if (Blynk.connected()) {
      Blynk.virtualWrite(V_ANOMALY_LED, 255);
      Blynk.logEvent("environmental_anomaly",
        String(r.label) + " deviates from building average (T:" +
        String(r.temperature, 1) + "C, L:" + String(r.light));
    }
    return true;
  }
  if (Blynk.connected()) {
    Blynk.virtualWrite(V_ANOMALY_LED, 0);
  }
  return false;
}

// ------------------------------------------------------------------------------
// APDS9960 gesture handling used as the physical UI:
//   UP    -> previous room preset
//   DOWN  -> next room preset
//   RIGHT -> confirm label (writes to Blynk label + OLED)
//   LEFT  -> trigger a Wi-Fi scan
//   NEAR (proximity only, no swipe) -> force manual measurement
// ------------------------------------------------------------------------------
void handleGesture() {
  // Adafruit_APDS9960 has no separate "gesture available" check, and no
  // APDS9960_NONE constant — readGesture() simply returns 0 when there's
  // no gesture ready (the real directions are APDS9960_UP/DOWN/LEFT/RIGHT).
  uint8_t gesture = apds.readGesture();
  if (gesture != 0) {
    switch (gesture) {
      case APDS9960_UP:
        presetIndex = (presetIndex - 1 + NUM_PRESETS) % NUM_PRESETS;
        if (Blynk.connected()) Blynk.virtualWrite(V_ROOM_LABEL, ROOM_PRESETS[presetIndex]);
        break;
      case APDS9960_DOWN:
        presetIndex = (presetIndex + 1) % NUM_PRESETS;
        if (Blynk.connected()) Blynk.virtualWrite(V_ROOM_LABEL, ROOM_PRESETS[presetIndex]);
        break;
      case APDS9960_RIGHT:
        logEvent(String("Room label confirmed: ") + ROOM_PRESETS[presetIndex]);
        if (Blynk.connected()) Blynk.virtualWrite(V_ROOM_LABEL, ROOM_PRESETS[presetIndex]);
        break;
      case APDS9960_LEFT:
        // Swipe left = scan for nearby Wi-Fi networks on demand
        if (!wifiScanInProgress) startWifiScan();
        break;
      default:
        break;
    }
  }
}

// ------------------------------------------------------------------------------
// Blynk button (V_MANUAL_BTN): lets the user in the app force a measurement
// even if the walking-detection state machine hasn't settled yet.
// ------------------------------------------------------------------------------
BLYNK_WRITE(V_MANUAL_BTN) {
  int pressed = param.asInt();
  if (pressed == 1 && currentState == STATE_IDLE) {
    currentState = STATE_RECORDING;
  }
}

// Blynk button (V_SCAN_BTN): lets the user in the app force a Wi-Fi rescan
BLYNK_WRITE(V_SCAN_BTN) {
  int pressed = param.asInt();
  if (pressed == 1 && !wifiScanInProgress) {
    startWifiScan();
  }
}

// Optional: let the app rename the current preset directly via a text input
BLYNK_WRITE(V_ROOM_LABEL) {
  String newLabel = param.asStr();
  if (newLabel.length() > 0 && roomCount < MAX_ROOMS) {
    strncpy(rooms[roomCount == 0 ? 0 : roomCount - 1].label, newLabel.c_str(), 19);
  }
}

// ------------------------------------------------------------------------------
// Push a completed reading's values out to the Blynk dashboard widgets.
// Self-guards on connectivity: no-ops and returns false if the cloud link
// isn't up right now, so callers never need to check first.
// ------------------------------------------------------------------------------
bool pushToBlynk(RoomReading &r) {
  if (!Blynk.connected()) return false;
  Blynk.virtualWrite(V_TEMP, r.temperature);
  Blynk.virtualWrite(V_PRESSURE, r.pressure);
  Blynk.virtualWrite(V_FLOOR, r.floorLevel);
  Blynk.virtualWrite(V_LIGHT, r.light);
  Blynk.virtualWrite(V_LIGHT_R, r.lightR);
  Blynk.virtualWrite(V_LIGHT_G, r.lightG);
  Blynk.virtualWrite(V_LIGHT_B, r.lightB);
  Blynk.virtualWrite(V_ALS_VALID, r.alsValid ? 255 : 0);
  Blynk.virtualWrite(V_COMFORT, r.comfortScore);
  Blynk.virtualWrite(V_ROOM_COUNT, roomCount);
  return true;
}

void logEvent(const String &msg) {
  Serial.println(msg); // always logged locally, network or not
  if (Blynk.connected()) {
    Blynk.virtualWrite(V_LOG, msg);
  }
}

// ------------------------------------------------------------------------------
// BLUETOOTH LOW ENERGY — sets up a GATT server that broadcasts the same data
// Blynk gets, over BLE, so a companion phone app can receive it without any
// internet/Wi-Fi router in range. Runs concurrently with the Wi-Fi/Blynk link.
// ------------------------------------------------------------------------------
void setupBLE() {
  BLEDevice::init("IntelliMap");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BLEConnectionCallbacks());

  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pDataCharacteristic = pService->createCharacteristic(
      BLE_DATA_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pDataCharacteristic->addDescriptor(new BLE2902());

  pNetworkCharacteristic = pService->createCharacteristic(
      BLE_NETWORK_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pNetworkCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  pServer->getAdvertising()->start();
}

// Sends one room reading out as a compact JSON string over BLE notify.
void bleBroadcastReading(RoomReading &r) {
  String json = "{";
  json += "\"room\":\"" + String(r.label) + "\",";
  json += "\"temp\":" + String(r.temperature, 1) + ",";
  json += "\"pressure\":" + String(r.pressure, 1) + ",";
  json += "\"floor\":" + String(r.floorLevel) + ",";
  json += "\"light\":" + String(r.light) + ",";
  json += "\"lightR\":" + String(r.lightR) + ",";
  json += "\"lightG\":" + String(r.lightG) + ",";
  json += "\"lightB\":" + String(r.lightB) + ",";
  json += "\"alsValid\":" + String(r.alsValid ? "true" : "false") + ",";
  json += "\"comfort\":" + String(r.comfortScore) + ",";
  json += "\"anomaly\":" + String(r.anomaly ? "true" : "false");
  json += "}";

  pDataCharacteristic->setValue(json.c_str());
  if (bleClientConnected) pDataCharacteristic->notify(); // BLE works with zero Wi-Fi
  if (Blynk.connected()) Blynk.virtualWrite(V_BLE_STATUS, bleClientConnected ? 255 : 0);
}

// ------------------------------------------------------------------------------
// WI-FI NETWORK SCANNING — detects every access point in range. Runs async
// (non-blocking) so it never stalls the state machine or sensor sampling.
// ------------------------------------------------------------------------------
void startWifiScan() {
  wifiScanInProgress = true;
  lastWifiScanTime = millis();
  WiFi.scanNetworks(true); // true = async: returns immediately
  logEvent("Scanning for nearby Wi-Fi networks...");
}

void checkWifiScan() {
  if (!wifiScanInProgress) return;

  int result = WiFi.scanComplete(); // -1 = still running, -2 = not started
  if (result == WIFI_SCAN_RUNNING || result == WIFI_SCAN_FAILED) return;

  int count = result;
  lastNetworkCount = count;

  // Sort indices by RSSI (strongest first) without disturbing WiFi's internal list
  int idx[32];
  int n = min(count, 32);
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) {
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
      }
    }
  }

  String summary = "";
  String bleNetJson = "[";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(idx[i]);
    if (ssid.length() == 0) ssid = "(hidden)";
    int rssi = WiFi.RSSI(idx[i]);
    bool secure = WiFi.encryptionType(idx[i]) != WIFI_AUTH_OPEN;

    summary += ssid + " (" + String(rssi) + "dBm" + (secure ? ", locked" : ", open") + ")\n";

    bleNetJson += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) +
                  ",\"secure\":" + String(secure ? "true" : "false") + "}";
    if (i < n - 1) bleNetJson += ",";
  }
  bleNetJson += "]";
  lastNetworkSummary = summary;

  if (Blynk.connected()) {
    Blynk.virtualWrite(V_NET_COUNT, count);
    Blynk.virtualWrite(V_NET_LOG, summary.length() ? summary : String("No networks found"));
  }
  pNetworkCharacteristic->setValue(bleNetJson.c_str()); // BLE, always
  if (bleClientConnected) pNetworkCharacteristic->notify();

  logEvent(String(count) + " Wi-Fi network(s) detected nearby.");

  WiFi.scanDelete();
  wifiScanInProgress = false;
  wifiScanPending = true; // shown as soon as the OLED is back at idle
}

// ------------------------------------------------------------------------------
// CONNECTIVITY MANAGER — the only place in this sketch that talks to Wi-Fi
// or the Blynk cloud. Fully non-blocking: wifiMulti.run() and Blynk.connect()
// are both given short bounded timeouts, so a dead network never stalls
// sensors, gestures, the OLED, or BLE. Called once per loop(); does almost
// nothing on most calls. wifiMulti.run() automatically picks whichever of
// the up-to-4 configured networks is strongest, and fails over to another
// configured network on its own if the current one drops.
// ------------------------------------------------------------------------------
void maintainConnectivity() {
  unsigned long now = millis();

  if (!wifiConnected) {
    // No network yet: retry periodically, never block waiting.
    if (now - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttempt = now;
      if (wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS) == WL_CONNECTED) {
        wifiConnected = true;
        // Keep the radio fully awake once connected — the ESP32's default
        // Wi-Fi modem-sleep periodically powers the radio down between
        // beacon intervals to save power, which adds latency and can cause
        // drops for a device that's meant to be on the network continuously.
        // setAutoReconnect() also makes the driver retry on its own the
        // instant a link is lost, on top of the wifiMulti retry loop above.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        logEvent("Wi-Fi connected: " + WiFi.SSID() + " (" +
                 WiFi.localIP().toString() + ")");
      }
    }
    return; // nothing more to do until Wi-Fi comes back
  }

  // Already connected — confirm it's still actually up. If it drops,
  // background scanning for nearby networks (in loop()) resumes on its
  // own since that's gated on !wifiConnected.
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    Serial.println("Wi-Fi lost — continuing fully offline.");
    return;
  }

  if (Blynk.connected()) {
    Blynk.run();
    wasBlynkConnected = true;
    return;
  }

  if (wasBlynkConnected) {
    Serial.println("Blynk cloud lost — continuing offline, readings still buffered.");
    wasBlynkConnected = false;
  }

  // Wi-Fi is up but the cloud isn't — try again periodically, with a short
  // bounded timeout so even a failed attempt can't stall the device long.
  if (now - lastBlynkAttempt > BLYNK_RETRY_INTERVAL_MS) {
    lastBlynkAttempt = now;
    if (Blynk.connect(BLYNK_CONNECT_TIMEOUT_MS)) {
      logEvent("Blynk cloud connected — syncing offline data.");
      wasBlynkConnected = true;
      syncPendingReadings();
    }
  }
}

// ------------------------------------------------------------------------------
// Replays any readings captured while the cloud link was down. Called
// automatically the moment Blynk reconnects. Gauges only show the latest
// value, so we push a text log line per missed room plus a final gauge
// update to bring the dashboard's live numbers up to date.
// ------------------------------------------------------------------------------
void syncPendingReadings() {
  int syncedCount = 0;
  for (int i = 0; i < roomCount; i++) {
    if (!rooms[i].synced) {
      RoomReading &r = rooms[i];
      String msg = "[offline sync] " + String(r.label) + ": " +
                   String(r.temperature, 1) + "C, " + String(r.pressure, 1) +
                   "hPa, floor " + String(r.floorLevel) + ", light C:" +
                   String(r.light) + " R:" + String(r.lightR) + " G:" +
                   String(r.lightG) + " B:" + String(r.lightB) +
                   ", score " + String(r.comfortScore);
      Blynk.virtualWrite(V_LOG, msg);
      rooms[i].synced = true;
      syncedCount++;
    }
  }
  if (syncedCount > 0) {
    pushToBlynk(rooms[roomCount - 1]); // bring gauges up to the latest reading
    logEvent(String(syncedCount) + " offline reading(s) synced to Blynk.");
  }
}

// ------------------------------------------------------------------------------
// OLED screens
// ------------------------------------------------------------------------------
void drawIdleScreen(float variance) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println(F("IntelliMap - Scanning"));
  display.print(F("Room: "));
  display.println(ROOM_PRESETS[presetIndex]);
  display.print(F("Motion var: "));
  display.println(variance, 2);
  display.println(variance < MOTION_VARIANCE_THRESHOLD ?
                   F("Status: STILL") : F("Status: WALKING"));
  display.print(F("Rooms logged: "));
  display.println(roomCount);
  display.print(F("Net: "));
  if (!wifiConnected) {
    display.println(F("offline"));
  } else if (Blynk.connected()) {
    display.println(F("Wi-Fi+Cloud"));
  } else {
    display.println(F("Wi-Fi only"));
  }
  display.display();
}

void drawCountdownScreen(unsigned long remainingMs) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println(F("Hold still to record..."));
  display.setTextSize(3);
  display.setCursor(40, 24);
  display.println((remainingMs / 1000) + 1);
  display.display();
}

void drawResultScreen(RoomReading &r) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println(r.label);
  display.print(F("Temp: "));
  display.print(r.temperature, 1);
  display.println(F(" C"));
  display.print(F("Pressure: "));
  display.print(r.pressure, 1);
  display.println(F(" hPa"));
  display.print(F("Floor: "));
  display.println(r.floorLevel);
  display.print(F("Clear: "));
  display.print(r.light);
  if (!r.alsValid) display.println(F("(ALS stale)"));
  display.print(F("Comfort: "));
  display.print(r.comfortScore);
  display.println(F("%"));
  if (r.anomaly) display.println(F("** ANOMALY DETECTED **"));
  display.display();
}

void drawWifiScanScreen() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.print(F("Networks found: "));
  display.println(lastNetworkCount);
  display.println(F("------------------"));

  // Print up to MAX_NETWORKS_SHOWN lines from the cached summary string
  int shown = 0;
  int start = 0;
  while (shown < MAX_NETWORKS_SHOWN) {
    int nl = lastNetworkSummary.indexOf('\n', start);
    if (nl == -1) break;
    String line = lastNetworkSummary.substring(start, nl);
    if (line.length() > 21) line = line.substring(0, 21); // fit 128px width
    display.println(line);
    start = nl + 1;
    shown++;
  }
  display.display();
}
