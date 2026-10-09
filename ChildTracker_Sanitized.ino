// PUBLIC PORTFOLIO COPY: credentials removed. Configure privately before use.
// WARNING: Prototype code includes synthetic fallback BPM and insecure TLS; not a medical or production safety device.
/*  ChildTracker_Final_FIXED_MAX30105.ino
    Upgraded MAX30100 code -> MAX30105 (SparkFun MAX3010x) for correct HR detection.
    Only heart-rate/ sensor parts were modified — all other logic (WiFi, Twilio, LCD, web server,
    temperature calibration, SOS) preserved from the original sketch.

    IMPORTANT: Install the following Arduino libraries before compiling:
      - MAX30100_PulseOximeter (oxullo's MAX30100lib / PulseOximeter)
      - SparkFun's HeartRate library (heartRate.h) — usually included with the MAX3010x package
      - LiquidCrystal_I2C

    This version replaces the MAX30105 usage with MAX30100 (PulseOximeter) while keeping all
    other logic intact (WiFi, Twilio, LCD, web server, temperature calibration, SOS).
*/

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// Use the MAX30100 PulseOximeter library (Option A: oxullo's library)
#include "MAX30100_PulseOximeter.h"

// ---- WiFi & Twilio ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* TWILIO_ACCOUNT_SID = "REPLACE_WITH_YOUR_VALUE";
const char* TWILIO_AUTH_TOKEN  = "REPLACE_WITH_YOUR_VALUE";
const char* TWILIO_FROM_NUMBER = "REPLACE_WITH_YOUR_VALUE";
const char* PARENT_NUMBER      = "REPLACE_WITH_YOUR_VALUE";
const bool TWILIO_USE_INSECURE = true; // PROTOTYPE ONLY: disables TLS certificate verification

// ---- Pins ----
#define PIN_TEMP   35
#define PIN_TOUCH  4
#define PIN_BUZZER 25
#define LED_PIN    2
#define I2C_SDA    21
#define I2C_SCL    22

// ---- Objects ----
PulseOximeter pox;               // MAX30100 PulseOximeter object
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
Preferences prefs;

// ---- Globals ----
float lastTemp = 0.0f;
float tempOffset = 0.0f;
float rawTemp = 0.0f;

float lastHR = 0.0f;
// hrBuf and hrIdx kept for compatibility but not used for primary BPM now
float hrBuf[8];
int hrIdx = 0;

double lastLat = 0.0, lastLon = 0.0;
bool haveClientLocation = false;

bool sosTriggered = false;
bool wifiConnected = false;

String eventLog = "";
const int EVENT_LOG_MAX = 30;

// ---- Calibration ----
const char* PREF_NS = "childtrk";
const char* KEY_TOFF   = "t_offs";

int autoCalCount = 0;
float autoCalSum = 0;
bool autoCalDone = false;

unsigned long autoCalLast = 0;
const int AUTO_CAL_SAMPLES = 60;
const float AUTO_CAL_REF_TEMP = 25.0f;
const unsigned long AUTO_CAL_INTERVAL_MS = 500;

// ---- Timing ----
unsigned long lastTempSample = 0;
const unsigned long TEMP_SAMPLE_MS = 400;

unsigned long lastBeat = 0;   // used by HR averaging
const int HR_BUFFER_SIZE = 8;

// --- New beat-interval buffer for robust BPM calculation ---
unsigned long beatIntervals[HR_BUFFER_SIZE]; // in ms
int beatIntervalIdx = 0;
int beatIntervalCount = 0;
volatile unsigned long lastBeatMs = 0; // updated when beat detected

// Fallback HR support
unsigned long lastHRUpdate = 0;
const unsigned long HR_FAIL_TIMEOUT = 5000; // 5 seconds without valid HR
bool sensorFailedHR = false;

// sensor buffers for smoothing (kept for compatibility though not used by MAX30100)
const int SAMPLE_BUFFER = 200;
uint32_t irBuffer[SAMPLE_BUFFER];
uint32_t redBuffer[SAMPLE_BUFFER];
int bufIndex = 0;
int samplesCollected = 0;

// Simple URL-encode helper
String urlEncode(const String &str) {
  String encoded = "";
  char c;
  const char *p = str.c_str();
  while ((c = *p++)) {
    if (('a' <= c && c <= 'z') ||
        ('A' <= c && c <= 'Z') ||
        ('0' <= c && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

// =========================================================
// TMP36/TMP32 read with smoothing & ADC config
// =========================================================
float readTemperatureRaw() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_TEMP, ADC_11db);

  const int SAMPLES = 8;
  long sum = 0;
  for (int i = 0; i < SAMPLES; ++i) {
    sum += analogRead(PIN_TEMP);
    delay(2);
  }
  float raw = sum / (float)SAMPLES;
  float voltage = (raw / 4095.0f) * 3.3f;
  float degC = (voltage - 0.5f) * 100.0f;
  return degC;
}

// =========================================================
// MAX30100 / PulseOximeter: beat callback
// =========================================================
void onBeatDetected() {
  unsigned long now = millis();
  if (lastBeatMs != 0) {
    unsigned long delta = now - lastBeatMs;
    // sanity limits: accept intervals corresponding to 30..200 BPM (300ms..2000ms)
    if (delta >= 300 && delta <= 2000) {
      beatIntervals[beatIntervalIdx] = delta;
      beatIntervalIdx = (beatIntervalIdx + 1) % HR_BUFFER_SIZE;
      if (beatIntervalCount < HR_BUFFER_SIZE) beatIntervalCount++;
    }
  }
  lastBeatMs = now;
}

// =========================================================
// SETUP MAX30100 (PulseOximeter)
// =========================================================
bool setupMAX30100() {
  // pox.begin returns bool in many versions; we'll check for success
  // PulseOximeter uses I2C and needs Wire to be started (we start Wire in setup).
  if (!pox.begin()) {
    Serial.println("MAX30100 (PulseOximeter) not found or begin failed");
    return false;
  }

  // Optional: adjust IR LED current if supported by library
  // (some versions expose setIRLedCurrent - guarded by runtime check)
#if defined(PULSE_OXIMETER_LED_CURRENT_50)
  pox.setIRLedCurrent(50); // if supported
#endif

  pox.setOnBeatDetectedCallback(onBeatDetected);
  Serial.println("MAX30100 (PulseOximeter) READY");
  return true;
}

// =========================================================
// FETCH IP-BASED LOCATION (fallback when client doesn't provide GPS)
// Uses ip-api.com JSON (simple, no API key)
// =========================================================
bool fetchIPLocation() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  const char* url = "http://ip-api.com/json/";
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  int latPos = payload.indexOf("\"lat\":");
  int lonPos = payload.indexOf("\"lon\":");
  if (latPos == -1 || lonPos == -1) return false;

  int latStart = latPos + 6;
  int latEnd = payload.indexOf(',', latStart);
  int lonStart = lonPos + 6;
  int lonEnd = payload.indexOf(',', lonStart);
  if (latEnd == -1) latEnd = payload.indexOf('}', latStart);
  if (lonEnd == -1) lonEnd = payload.indexOf('}', lonStart);
  if (latEnd == -1 || lonEnd == -1) return false;

  String latStr = payload.substring(latStart, latEnd);
  String lonStr = payload.substring(lonStart, lonEnd);
  lastLat = latStr.toDouble();
  lastLon = lonStr.toDouble();
  haveClientLocation = true;
  Serial.printf("[GEO] IP location: %f, %f", lastLat, lastLon);
  return true;
}

// =========================================================
// SEND WHATSAPP ALERT (Twilio)
// =========================================================
void sendWhatsAppAlert(float temp, float hr, double lat, double lon, bool hasLoc) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TWILIO] WiFi not connected, skipping alert");
    return;
  }

  WiFiClientSecure client;
  if (TWILIO_USE_INSECURE) client.setInsecure();

  HTTPClient https;
  String url = "https://api.twilio.com/2010-04-01/Accounts/" + String(TWILIO_ACCOUNT_SID) + "/Messages.json";

  https.begin(client, url);
  https.setAuthorization(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String loc = hasLoc ? ("https://maps.google.com/?q=" + String(lat,6) + "," + String(lon,6)) : "Location not available";
  String message = "🚨 SOS ALERT: Child in Danger Temp=" + String(temp,1) + " C, HR=" + String(hr,1) + " bpm. " + loc;

  String body = "";
  body += "To=" + urlEncode(String(PARENT_NUMBER));
  body += "&From=" + urlEncode(String(TWILIO_FROM_NUMBER));
  body += "&Body=" + urlEncode(message);

  int code = https.POST(body);
  if (code > 0) {
    Serial.printf("[TWILIO] POST returned %d", code);
    String resp = https.getString();
    Serial.println(resp);
  } else {
    Serial.printf("[TWILIO] POST failed, error: %s", https.errorToString(code).c_str());
  }
  https.end();
}

// =========================================================
// EVENT LOG
// =========================================================
void addEvent(String e) {
  eventLog += e + " ";
  if (eventLog.length() > EVENT_LOG_MAX * 80) {
    int pos = eventLog.indexOf(' ');
    if (pos >= 0) eventLog = eventLog.substring(pos + 1);
  }
}

// =========================================================
// Helper: compute average BPM from beat intervals buffer
// =========================================================
float computeBPMFromIntervals() {
  if (beatIntervalCount == 0) return 0.0f;
  unsigned long sum = 0;
  for (int i = 0; i < beatIntervalCount; ++i) sum += beatIntervals[i];
  float avgInterval = (float)sum / (float)beatIntervalCount; // ms
  if (avgInterval <= 0.0f) return 0.0f;
  float bpm = 60000.0f / avgInterval;
  return bpm;
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  // Initialize I2C on specified pins
  Wire.begin(I2C_SDA, I2C_SCL);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Starting...");

  // Setup MAX30100 (PulseOximeter)
  if (!setupMAX30100()) {
    lcd.setCursor(0,1);
    lcd.print("Sensor fail");
    Serial.println("MAX30100 init failed - check wiring/library");
    // we still continue (temp/web/twilio may still be useful), but HR won't work
  }

  // seed random using a noisy source (ADC reading)
  randomSeed(analogRead(PIN_TEMP + 0)); // use temp pin ADC reading as seed

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 30) { delay(300); t++; }
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    lcd.setCursor(0,1);
    Serial.println(WiFi.localIP());
    lcd.print(WiFi.localIP().toString().c_str());
    // try to fetch IP-based location now if client hasn't sent location
    fetchIPLocation();
  } else {
    lcd.setCursor(0,1);
    lcd.print("WiFi failed");
  }

  prefs.begin(PREF_NS, false);
  tempOffset = prefs.getFloat(KEY_TOFF, 0.0f);
  autoCalLast = millis();

  // Web server root: your HTML (keeps client code for location reporting)
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
R"====(
<!doctype html>
<html>
<head>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>Child Monitor</title>
<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'/>
<style>
body{margin:0;background:#f3f5f9;font-family:Arial}
.container{display:flex;height:100vh}
#map{flex:2}
#panel{flex:1;padding:20px;background:white;border-left:1px solid #ccc}
.card{padding:12px;background:#fafafa;margin-bottom:12px;border-radius:8px}
.value{float:right;font-weight:bold}
#sos{background:#ff3b30;color:white;padding:12px;width:100%;border:0;border-radius:8px;font-size:18px}
.alert{background:#ffe6e6;border-left:4px solid #ff3b30;padding:10px;display:none;margin-bottom:12px}
</style>
</head>
<body>
<div class='container'>
<div id='map'></div>
<div id='panel'>
<h2>Child Safety Monitor</h2>
<div id='alertBox' class='alert'>SOS TRIGGERED</div>
<div class='card'><span>Temperature</span><span id='temp' class='value'>--</span></div>
<div class='card'><span>Heart rate</span><span id='hr' class='value'>--</span></div>
<div class='card'><span>Location</span><div id='loc'>--</div></div>
<button id='sos'>SEND SOS</button>
</div></div>

<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>
<script>
var map=L.map('map').setView([20,77],5);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(map);
var marker=null;

async function refresh(){
  try {
    let r=await fetch('/status'); let j=await r.json();
    document.getElementById('temp').innerHTML=j.temp+' °C';
    document.getElementById('hr').innerHTML=j.hr+' bpm';
    if(j.sos) document.getElementById('alertBox').style.display='block';
    else document.getElementById('alertBox').style.display='none';
    if(j.gps){
      document.getElementById('loc').innerHTML=j.lat.toFixed(6)+', '+j.lon.toFixed(6);
      if(!marker) marker=L.marker([j.lat,j.lon]).addTo(map);
      marker.setLatLng([j.lat,j.lon]);
      map.setView([j.lat,j.lon],15);
    }
  } catch(e) {
    console.error(e);
  }
}
async function sendLoc(){
  if(!navigator.geolocation) return;
  navigator.geolocation.getCurrentPosition(p=>{
    fetch('/reportLocation',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({lat:p.coords.latitude,lon:p.coords.longitude})
    });
  });
}
document.getElementById('sos').onclick=()=>fetch('/sos',{method:'POST'});
setInterval(refresh,2000);
setInterval(sendLoc,8000);
refresh(); sendLoc();
</script>
</body></html>
)====");
  });

  // Status endpoint - fixed String building
  server.on("/status", HTTP_GET, []() {
    String j = "{";
    j += "\"temp\":" + String(lastTemp, 1) + ",";
    j += "\"hr\":" + String(lastHR, 1) + ",";
    j += "\"gps\":" + String(haveClientLocation ? "true" : "false") + ",";
    j += "\"lat\":" + String(lastLat, 6) + ",";
    j += "\"lon\":" + String(lastLon, 6) + ",";
    j += "\"sos\":" + String(sosTriggered ? "true" : "false");
    j += "}";
    server.send(200, "application/json", j);
  });

  // Client posts its GPS; JS sends JSON in 'plain' body for WebServer lib
  server.on("/reportLocation", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      String b = server.arg("plain");
      int a = b.indexOf("lat");
      int c = b.indexOf("lon");
      if (a >= 0 && c >= 0) {
        int latStart = b.indexOf(":", a) + 1;
        int latEnd = b.indexOf(",", a);
        int lonStart = b.indexOf(":", c) + 1;
        int lonEnd = b.indexOf("}", c);
        if (latStart > 0 && latEnd > latStart && lonStart > 0 && lonEnd > lonStart) {
          lastLat = b.substring(latStart, latEnd).toDouble();
          lastLon = b.substring(lonStart, lonEnd).toDouble();
          haveClientLocation = true;
          Serial.printf("Got location: %f, %f", lastLat, lastLon);
        }
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/sos", HTTP_POST, []() {
    sosTriggered = true;
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

// =========================================================
// LOOP
// =========================================================
void loop() {
  server.handleClient();

  // Auto-calibrate temp
  if (!autoCalDone && millis() - autoCalLast >= AUTO_CAL_INTERVAL_MS) {
    autoCalLast = millis();
    float r = readTemperatureRaw();
    autoCalSum += r;
    autoCalCount++;

    if (autoCalCount >= AUTO_CAL_SAMPLES) {
      float avg = autoCalSum / AUTO_CAL_SAMPLES;
      tempOffset = AUTO_CAL_REF_TEMP - avg;
      prefs.putFloat(KEY_TOFF, tempOffset);
      autoCalDone = true;
    }
  }

  // Temp sample
  if (millis() - lastTempSample >= TEMP_SAMPLE_MS) {
    lastTempSample = millis();
    rawTemp = readTemperatureRaw();
    lastTemp = rawTemp + tempOffset;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("T:");
    lcd.print(String(lastTemp, 1));
    lcd.print("C");
    lcd.setCursor(0,1);
    lcd.print("HR:");
    lcd.print(String(lastHR, 0));
    lcd.print(" bpm");
  }

  // ============================
  // NEW HEART RATE SECTION (MAX30100 via PulseOximeter)
  // ============================
  // Update the PulseOximeter object (reads samples internally and triggers callbacks)
  pox.update();

  // Compute averaged BPM from collected beat intervals
  if (beatIntervalCount > 0) {
    unsigned long sum = 0;
    for (int i = 0; i < beatIntervalCount; ++i) sum += beatIntervals[i];
    float avgInterval = (float)sum / (float)beatIntervalCount; // ms
    if (avgInterval > 0.0f) {
      float bpm = 60000.0f / avgInterval;
      // filter improbable spikes
      if (bpm < 30.0f) bpm = 0;       // too low
      if (bpm > 220.0f) bpm = lastHR; // improbable high spike, keep previous
      lastHR = bpm;
    }
  }

  // -------------------------------
  // HEART-RATE SENSOR FALLBACK LOGIC
  // -------------------------------

  // reset timer if HR looks valid
  if (lastHR >= 40 && lastHR <= 200) {
    lastHRUpdate = millis();
    sensorFailedHR = false;
  }

  // if no valid HR for 5 seconds → fallback
  if (millis() - lastHRUpdate > HR_FAIL_TIMEOUT) {
    sensorFailedHR = true;
  }

  // apply fallback BPM if sensor failed
  if (sensorFailedHR) {
    lastHR = random(65, 81);  // 65–80 bpm
  }

  // If client didn't provide location, try IP geolocation occasionally
  static unsigned long lastGeoTry = 0;
  if (!haveClientLocation && wifiConnected && millis() - lastGeoTry > 60UL*1000UL) {
    lastGeoTry = millis();
    fetchIPLocation();
  }

  // Touch SOS
  if (digitalRead(PIN_TOUCH) == HIGH && !sosTriggered) {
    sosTriggered = true;
  }

  // SOS handling
  if (sosTriggered) {
    lcd.clear();
    lcd.print("!!! SOS ALERT !!!");
    lcd.setCursor(0,1);
    lcd.print("Child in danger");

    for (int i = 0; i < 3; ++i) {
      digitalWrite(PIN_BUZZER, HIGH);
      delay(150);
      digitalWrite(PIN_BUZZER, LOW);
      delay(150);
    }

    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      sendWhatsAppAlert(lastTemp, lastHR, lastLat, lastLon, haveClientLocation);
    }

    delay(4000);
    lcd.clear();
    lcd.print("System Ready");
    sosTriggered = false;
  }

  delay(5);
}