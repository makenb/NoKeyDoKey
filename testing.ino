
// esp32_keyless_entry_async.ino
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Preferences.h>
#include <Update.h>

#define NUM_CHANNELS 4
// RF receiver pins - adjusted for RX480-E4 mapping (K1→D0, K2→D1, K3→D3, K4→D2)
const int rfPins[NUM_CHANNELS] = {32, 33, 26, 25}; // D0, D1, D3, D2 pins
const int relayPins[NUM_CHANNELS] = {16, 17, 18, 19};
const int vtPin = 27; // VT pin - valid transmission indicator (optional)

AsyncWebServer server(80);
Preferences prefs;

// Press handling with debouncing for RF signals
unsigned long pressStart[NUM_CHANNELS];
unsigned long lastPressTime[NUM_CHANNELS];
unsigned long lastStateChange[NUM_CHANNELS];
bool isPressing[NUM_CHANNELS] = {false};
bool doublePending[NUM_CHANNELS] = {false};
bool lastState[NUM_CHANNELS] = {false};

// Non-blocking relay control
unsigned long relayStartTime[NUM_CHANNELS];
bool relayActive[NUM_CHANNELS] = {false};
unsigned long relayDuration[NUM_CHANNELS]; // Configurable per relay

const unsigned long SHORT_PRESS_MAX = 400;
const unsigned long LONG_PRESS_MIN = 800;
const unsigned long DOUBLE_PRESS_GAP = 500;
const unsigned long DEFAULT_RELAY_DURATION = 300;
const unsigned long DEBOUNCE_TIME = 50; // RF signal debouncing

// Mapping matrix [channel][press_type] => relayAction
// press_type: 0 = short, 1 = long, 2 = double
String pressTypes[3] = {"short", "long", "double"};
String actions[NUM_CHANNELS][3];

const char* ssid = "ESP32_Keyless";
const char* password = "12345678";

void setup() {
  Serial.begin(115200);

  WiFi.softAP(ssid, password);
  Serial.println("AP started. IP: " + WiFi.softAPIP().toString());

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(rfPins[i], INPUT);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
    relayActive[i] = false;
    lastState[i] = false;
    lastStateChange[i] = 0;
  }
  
  // Optional: VT pin for valid transmission detection
  // pinMode(vtPin, INPUT);

  // Load config
  prefs.begin("relaycfg", true);
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    // Load relay durations (default 300ms)
    String durKey = "relay" + String(ch) + "dur";
    relayDuration[ch] = prefs.getULong(durKey.c_str(), DEFAULT_RELAY_DURATION);
    
    for (int pt = 0; pt < 3; pt++) {
      String key = String("c") + ch + pressTypes[pt];
      actions[ch][pt] = prefs.getString(key.c_str(), "none");
    }
  }
  prefs.end();

  // Setup async web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, handleUpdateForm);
  
  // Handle OTA update
  server.on("/update", HTTP_POST, 
    [](AsyncWebServerRequest *request) {
      bool shouldReboot = !Update.hasError();
      AsyncWebServerResponse *response = request->beginResponse(200, "text/html", 
        shouldReboot ? "Update Success! Rebooting..." : "Update Failed!");
      response->addHeader("Connection", "close");
      request->send(response);
      if (shouldReboot) {
        delay(1000);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        Serial.printf("Update Start: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (!Update.hasError()) {
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
      }
      if (final) {
        if (Update.end(true)) {
          Serial.printf("Update Success: %uB\n", index + len);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();
}

void loop() {
  unsigned long now = millis();

  // Handle relay timing (non-blocking)
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (relayActive[i] && (now - relayStartTime[i] >= relayDuration[i])) {
      digitalWrite(relayPins[i], LOW);
      relayActive[i] = false;
    }
  }

  // Handle RF input with debouncing
  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool currentState = digitalRead(rfPins[i]);
    
    // Debounce RF signal
    if (currentState != lastState[i]) {
      lastStateChange[i] = now;
      lastState[i] = currentState;
    }
    
    // Skip if still in debounce period
    if ((now - lastStateChange[i]) < DEBOUNCE_TIME) continue;
    
    bool state = currentState;
    
    if (state && !isPressing[i]) {
      pressStart[i] = now;
      isPressing[i] = true;
      Serial.printf("Channel %d pressed\n", i + 1);
    }
    if (!state && isPressing[i]) {
      unsigned long duration = now - pressStart[i];
      isPressing[i] = false;
      Serial.printf("Channel %d released after %lums\n", i + 1, duration);
      
      if (duration >= LONG_PRESS_MIN) {
        triggerAction(i, 1); // Long press
        doublePending[i] = false;
      } else if (duration <= SHORT_PRESS_MAX) {
        if (doublePending[i] && (now - lastPressTime[i] <= DOUBLE_PRESS_GAP)) {
          triggerAction(i, 2); // Double press
          doublePending[i] = false;
        } else {
          doublePending[i] = true;
          lastPressTime[i] = now;
        }
      }
    }
    if (doublePending[i] && (now - lastPressTime[i] > DOUBLE_PRESS_GAP)) {
      triggerAction(i, 0); // Single press
      doublePending[i] = false;
    }
  }
}

void triggerAction(int ch, int type) {
  String action = actions[ch][type];
  Serial.printf("Channel %d, %s press: %s\n", ch + 1, pressTypes[type].c_str(), action.c_str());
  if (action == "none") return;

  int relayIndex = action.toInt();
  if (relayIndex >= 0 && relayIndex < NUM_CHANNELS) {
    digitalWrite(relayPins[relayIndex], HIGH);
    relayActive[relayIndex] = true;
    relayStartTime[relayIndex] = millis();
  }
}

void handleRoot(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><title>ESP32 Keyless Config</title>";
  html += "<style>body{font-family:Arial;margin:20px;} h2{color:#333;} h3{color:#666;} ";
  html += "select,input[type=submit],input[type=number]{padding:5px;margin:5px;} ";
  html += "input[type=submit]{background:#007cba;color:white;border:none;cursor:pointer;} ";
  html += "input[type=submit]:hover{background:#005a8b;} ";
  html += ".relay-section{border:1px solid #ddd;padding:15px;margin:10px 0;border-radius:5px;} ";
  html += ".timing-input{width:80px;} label{display:inline-block;width:120px;}</style></head><body>";
  html += "<h2>ESP32 Keyless Entry Configuration</h2>";
  html += "<form method='POST' action='/save'>";
  
  // Relay timing configuration section
  html += "<div class='relay-section'>";
  html += "<h3>Relay Timing Configuration</h3>";
  for (int i = 0; i < NUM_CHANNELS; i++) {
    html += "<label>Relay " + String(i + 1) + " Duration:</label>";
    html += "<input type='number' name='relay" + String(i) + "dur' value='" + String(relayDuration[i]) + "' min='50' max='10000' class='timing-input'> ms<br><br>";
  }
  html += "</div>";
  
  // Channel mapping section
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    html += "<div class='relay-section'>";
    html += "<h3>Channel " + String(ch + 1) + " Button Mapping</h3>";
    for (int pt = 0; pt < 3; pt++) {
      html += "<label>" + pressTypes[pt] + " press:</label>";
      html += "<select name='c" + String(ch) + pressTypes[pt] + "'>";
      html += "<option value='none'" + (actions[ch][pt] == "none" ? " selected" : "") + ">None</option>";
      for (int i = 0; i < NUM_CHANNELS; i++) {
        String val = String(i);
        html += "<option value='" + val + "'" + (actions[ch][pt] == val ? " selected" : "") + ">Relay " + String(i + 1) + "</option>";
      }
      html += "</select><br><br>";
    }
    html += "</div>";
  }
  html += "<input type='submit' value='Save Configuration'>";
  html += "</form><br>";
  html += "<h3>Firmware Update</h3>";
  html += "<a href='/update' style='background:#28a745;color:white;padding:10px 15px;text-decoration:none;'>OTA Update</a>";
  html += "</body></html>";
  
  request->send(200, "text/html", html);
}

void handleSave(AsyncWebServerRequest *request) {
  prefs.begin("relaycfg", false);
  
  // Save relay durations
  for (int i = 0; i < NUM_CHANNELS; i++) {
    String durKey = "relay" + String(i) + "dur";
    if (request->hasParam(durKey, true)) {
      unsigned long duration = request->getParam(durKey, true)->value().toInt();
      // Validate range (50ms to 10 seconds)
      if (duration >= 50 && duration <= 10000) {
        relayDuration[i] = duration;
        prefs.putULong(durKey.c_str(), duration);
      }
    }
  }
  
  // Save channel mappings
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    for (int pt = 0; pt < 3; pt++) {
      String key = "c" + String(ch) + pressTypes[pt];
      if (request->hasParam(key, true)) {
        String val = request->getParam(key, true)->value();
        actions[ch][pt] = val;
        prefs.putString(key.c_str(), val);
      }
    }
  }
  prefs.end();
  
  String html = "<!DOCTYPE html><html><head><title>Configuration Saved</title>";
  html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
  html += "h3{color:#28a745;} a{background:#007cba;color:white;padding:10px 15px;text-decoration:none;}</style></head><body>";
  html += "<h3>Configuration Saved Successfully!</h3>";
  html += "<p>All relay timings and button mappings have been saved.</p>";
  html += "<a href='/'>Back to Configuration</a>";
  html += "</body></html>";
  
  request->send(200, "text/html", html);
}

void handleUpdateForm(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head><title>Firmware Update</title>";
  html += "<style>body{font-family:Arial;margin:20px;} ";
  html += "input[type=file],input[type=submit]{padding:10px;margin:10px;} ";
  html += "input[type=submit]{background:#28a745;color:white;border:none;cursor:pointer;} ";
  html += "input[type=submit]:hover{background:#1e7e34;}</style></head><body>";
  html += "<h2>Firmware Update</h2>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<p>Select firmware file (.bin):</p>";
  html += "<input type='file' name='update' accept='.bin'><br>";
  html += "<input type='submit' value='Upload & Update'>";
  html += "</form><br>";
  html += "<a href='/' style='color:#007cba;'>← Back to Configuration</a>";
  html += "</body></html>";
  
  request->send(200, "text/html", html);
}
