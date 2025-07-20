// esp32_keyless_entry.ino
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>

#define NUM_CHANNELS 4
const int rfPins[NUM_CHANNELS] = {32, 33, 25, 26};
const int relayPins[NUM_CHANNELS] = {16, 17, 18, 19};

WebServer server(80);
Preferences prefs;

// Press handling
unsigned long pressStart[NUM_CHANNELS];
unsigned long lastPressTime[NUM_CHANNELS];
bool isPressing[NUM_CHANNELS] = {false};
bool doublePending[NUM_CHANNELS] = {false};

const unsigned long SHORT_PRESS_MAX = 400;
const unsigned long LONG_PRESS_MIN = 800;
const unsigned long DOUBLE_PRESS_GAP = 500;

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
  }

  // Load config
  prefs.begin("relaycfg", true);
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    for (int pt = 0; pt < 3; pt++) {
      String key = String("c") + ch + pressTypes[pt];
      actions[ch][pt] = prefs.getString(key.c_str(), "none");
    }
  }
  prefs.end();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_POST, []() {
    server.send(200); }, handleOTA);
  server.on("/update", HTTP_GET, handleUpdateForm);
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool state = digitalRead(rfPins[i]);
    if (state && !isPressing[i]) {
      pressStart[i] = now;
      isPressing[i] = true;
    }
    if (!state && isPressing[i]) {
      unsigned long duration = now - pressStart[i];
      isPressing[i] = false;
      if (duration >= LONG_PRESS_MIN) {
        triggerAction(i, 1);
        doublePending[i] = false;
      } else if (duration <= SHORT_PRESS_MAX) {
        if (doublePending[i] && (now - lastPressTime[i] <= DOUBLE_PRESS_GAP)) {
          triggerAction(i, 2);
          doublePending[i] = false;
        } else {
          doublePending[i] = true;
          lastPressTime[i] = now;
        }
      }
    }
    if (doublePending[i] && (now - lastPressTime[i] > DOUBLE_PRESS_GAP)) {
      triggerAction(i, 0);
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
    delay(300);
    digitalWrite(relayPins[relayIndex], LOW);
  }
}

void handleRoot() {
  String html = "<html><body><h2>ESP32 Keyless Config</h2><form method='POST' action='/save'>";
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    html += "<h3>Channel " + String(ch + 1) + "</h3>";
    for (int pt = 0; pt < 3; pt++) {
      html += pressTypes[pt] + " press: <select name='c" + String(ch) + pressTypes[pt] + "'>";
      html += "<option value='none'" + (actions[ch][pt] == "none" ? " selected" : "") + ">None</option>";
      for (int i = 0; i < NUM_CHANNELS; i++) {
        String val = String(i);
        html += "<option value='" + val + "'" + (actions[ch][pt] == val ? " selected" : "") + ">Relay " + val + "</option>";
      }
      html += "</select><br>";
    }
  }
  html += "<input type='submit' value='Save'></form><br><a href='/update'>OTA Update</a></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  prefs.begin("relaycfg", false);
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    for (int pt = 0; pt < 3; pt++) {
      String key = "c" + String(ch) + pressTypes[pt];
      if (server.hasArg(key)) {
        String val = server.arg(key);
        actions[ch][pt] = val;
        prefs.putString(key.c_str(), val);
      }
    }
  }
  prefs.end();
  server.send(200, "text/html", "<html><body><h3>Saved! <a href='/'>Back</a></h3></body></html>");
}

void handleUpdateForm() {
  String html = "<form method='POST' action='/update' enctype='multipart/form-data'>"
                "Firmware: <input type='file' name='update'>"
                "<input type='submit' value='Update'></form>";
  server.send(200, "text/html", html);
}

void handleOTA() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
    else Update.printError(Serial);
    ESP.restart();
  }
}
