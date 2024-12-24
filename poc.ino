#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <WebServer.h>  // Classic Arduino-style web server for ESP32

// =====================
// EDIT THESE
// =====================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* apiKey   = "YOUR_GOOGLE_API_KEY";  // From https://aistudio.google.com/apikey

// The model you want to call once you have the file URI:
const char* modelName = "gemini-1.5-flash-8b";

// The user prompt we send to the AI model:
String userPrompt = "If there's a person in the image, set classification_result=true. Otherwise, classification_result=false.";

// The auto-classification interval (milliseconds):
unsigned long classificationInterval = 60000; // default = 60s

// LED pin to blink for 200ms if classification_result=true
#define LED_PIN 4

// ~~~~~ Camera pins: AI Thinker ~~~~~
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// =============================================================
// Global variables to store the last image and classification
// =============================================================
WiFiClientSecure client;  // for TLS

static uint8_t* lastImageBuf = nullptr;
static size_t   lastImageLen = 0;

static bool lastClassificationResult = false;

static int totalImages          = 0;
static int totalTrueClassified  = 0;

WebServer server(80);

// Forward declarations
String uploadImage(camera_fb_t *fb);
bool performClassification(const String& fileUri);
void triggerAlarm();
void sendDiscordAlert(); // optional

// --------------------------------------------------------------------------
// Setup camera
void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA; 
    config.jpeg_quality = 12; 
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 14; 
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    delay(1000);
    ESP.restart();
  }
}

// --------------------------------------------------------------------------
// Upload the image to Google and get back the fileUri
// --------------------------------------------------------------------------
String uploadImage(camera_fb_t *fb) {
  const char* host = "generativelanguage.googleapis.com";
  const int   httpsPort = 443;
  client.setInsecure();  // for simplicity

  if (!client.connect(host, httpsPort)) {
    Serial.println("Connection to Google failed");
    return "";
  }

  String boundary    = "====ESP32CAM_boundary====";
  String contentType = "multipart/related; boundary=" + boundary;

  String metadataJson =
    "{"
       "\"file\": {"
          "\"display_name\": \"esp32-cam.jpg\""
       "}"
    "}";

  String part1 =
    "--" + boundary + "\r\n"
    "Content-Type: application/json; charset=UTF-8\r\n\r\n"
    + metadataJson + "\r\n"
    "--" + boundary + "\r\n"
    "Content-Type: image/jpeg\r\n\r\n";

  String part2 =
    "\r\n--" + boundary + "--\r\n";

  uint32_t totalLength = part1.length() + fb->len + part2.length();

  String url = "/upload/v1beta/files?uploadType=multipart&key=" + String(apiKey);
  String request =
    "POST " + url + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "X-Goog-Upload-Command: start, upload, finalize\r\n" +
    "X-Goog-Upload-Header-Content-Length: " + String(fb->len) + "\r\n" +
    "X-Goog-Upload-Header-Content-Type: image/jpeg\r\n" +
    "Content-Type: " + contentType + "\r\n" +
    "Content-Length: " + String(totalLength) + "\r\n" +
    "Connection: close\r\n\r\n";

  client.print(request);
  client.print(part1);
  client.write(fb->buf, fb->len);
  client.print(part2);

  String body;
  bool headersEnded = false;
  unsigned long t0 = millis();
  while (client.connected() && (millis() - t0 < 5000)) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (!headersEnded && line == "\r") {
        headersEnded = true;
      } else if (headersEnded) {
        body += line + "\n";
      }
      t0 = millis();
    }
  }
  client.stop();

  Serial.println("Upload response:\n" + body);

  // Naive parse for "uri": "..."
  int idx = body.indexOf("\"uri\":");
  if (idx < 0) {
    Serial.println("No 'uri' found in response");
    return "";
  }
  int start = body.indexOf("\"", idx + 6) + 1;
  int end   = body.indexOf("\"", start);
  if (start < 0 || end < 0) {
    Serial.println("URI parse failure");
    return "";
  }
  String fileUri = body.substring(start, end);
  Serial.println("Parsed fileUri: " + fileUri);
  return fileUri;
}

// --------------------------------------------------------------------------
// Perform classification by calling generateContent on the uploaded file
// We'll parse "classification_result": true/false
// --------------------------------------------------------------------------
bool performClassification(const String& fileUri) {
  if (fileUri == "") {
    Serial.println("Empty fileUri, skipping");
    return false;
  }

  const char* host = "generativelanguage.googleapis.com";
  const int   httpsPort = 443;
  client.setInsecure(); // skip cert check

  if (!client.connect(host, httpsPort)) {
    Serial.println("Connection to Google failed (generateContent)");
    return false;
  }

  String body =
    "{"
      "\"contents\":["
        "{"
          "\"role\":\"user\","
          "\"parts\":["
            "{"
              "\"fileData\":{"
                "\"fileUri\":\"" + fileUri + "\","
                "\"mimeType\":\"image/jpeg\""
              "}"
            "},"
            "{"
              "\"text\":\"" + userPrompt + "\"}"
          "]"
        "}"
      "],"
      "\"generationConfig\":{"
        "\"temperature\":1,"
        "\"topK\":40,"
        "\"topP\":0.95,"
        "\"maxOutputTokens\":1024,"
        "\"responseMimeType\":\"application/json\","
        "\"responseSchema\":{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"classification_result\":{\"type\":\"boolean\"}"
          "},"
          "\"required\":[\"classification_result\"]"
        "}"
      "}"
    "}";

  String url = "/v1beta/models/" + String(modelName) + ":generateContent?key=" + String(apiKey);
  String request =
    "POST " + url + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "Content-Type: application/json\r\n" +
    "Content-Length: " + body.length() + "\r\n" +
    "Connection: close\r\n\r\n";

  client.print(request);
  client.print(body);

  String response;
  bool headersEnded = false;
  unsigned long t0 = millis();
  while (client.connected() && (millis() - t0 < 5000)) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (!headersEnded && line == "\r") {
        headersEnded = true;
      } else if (headersEnded) {
        response += line + "\n";
      }
      t0 = millis();
    }
  }
  client.stop();

  Serial.println("GenerateContent response:\n" + response);

  // Check if "classification_result" = true
  int truePos = response.indexOf("true", 0);
  if (truePos >= 0) {
    Serial.println("classification_result = true");
    return true;
  }
  Serial.println("classification_result = false");
  return false;
}

// --------------------------------------------------------------------------
// Trigger an "alarm" - in our example, flash an LED for 200ms
// --------------------------------------------------------------------------
void triggerAlarm() {
  Serial.println("Triggering Alarm!");
  digitalWrite(LED_PIN, HIGH);
  delay(200);
  digitalWrite(LED_PIN, LOW);
}

// --------------------------------------------------------------------------
// Serve the last image (if any) as /latest.jpg
// --------------------------------------------------------------------------
void handleLatestJPG() {
  if (lastImageBuf && lastImageLen > 0) {
    // no-cache headers
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send_P(200, "image/jpeg", (const char*)lastImageBuf, lastImageLen);
  } else {
    server.send(404, "text/plain", "No image yet");
  }
}

// --------------------------------------------------------------------------
// Capture a fresh image (HTMX callback)
// Returns partial HTML snippet with new <img> (cache-buster param)
// --------------------------------------------------------------------------
void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  if (lastImageBuf) {
    free(lastImageBuf);
    lastImageBuf = nullptr;
    lastImageLen = 0;
  }
  lastImageBuf = (uint8_t*) malloc(fb->len);
  if (!lastImageBuf) {
    esp_camera_fb_return(fb);
    server.send(500, "text/plain", "Not enough memory");
    return;
  }
  memcpy(lastImageBuf, fb->buf, fb->len);
  lastImageLen = fb->len;
  esp_camera_fb_return(fb);

  String html =
    "<img src=\"/latest.jpg?t=" + String(millis()) + "\" width=\"320\"/>";
  server.send(200, "text/html", html);
}

// --------------------------------------------------------------------------
// Classify the *already* captured last image, then return partial HTML snippet
// with classification & counters
// --------------------------------------------------------------------------
void handleClassify() {
  if (!lastImageBuf || lastImageLen == 0) {
    server.send(400, "text/plain", "No image in memory, capture first");
    return;
  }

  camera_fb_t fb;
  fb.buf    = lastImageBuf;
  fb.len    = lastImageLen;
  fb.width  = 0;
  fb.height = 0;
  fb.format = PIXFORMAT_JPEG;

  String fileUri = uploadImage(&fb);
  bool result = performClassification(fileUri);

  totalImages++;
  if (result) {
    totalTrueClassified++;
    triggerAlarm();
  }
  lastClassificationResult = result;

  String html =
    "<strong>classification_result = "
    + String(result ? "true" : "false")
    + "</strong><br/>"
    "Total images: " + String(totalImages)
    + " | Classified true: " + String(totalTrueClassified);

  server.send(200, "text/html", html);
}

// --------------------------------------------------------------------------
// Handle updating the classification interval
// --------------------------------------------------------------------------
void handleSetInterval() {
  if (server.hasArg("val")) {
    Serial.println(server.arg("val"));
    classificationInterval = server.arg("val").toInt();
    if (classificationInterval < 1000) classificationInterval = 1000; // safety
  }
  String html = "<p>Current interval (ms): <strong>" + String(classificationInterval) + "</strong></p>";
  server.send(200, "text/html", html);
}

// --------------------------------------------------------------------------
// Handle updating the user prompt
// --------------------------------------------------------------------------
void handleSetPrompt() {
  if (server.hasArg("text")) {
    userPrompt = server.arg("text");
    Serial.println(server.arg("text"));
  }
  server.send(200, "text/html", "<pre>" + userPrompt + "</pre>");
}

// --------------------------------------------------------------------------
// Reset counters
// --------------------------------------------------------------------------
void handleResetCounters() {
  totalImages = 0;
  totalTrueClassified = 0;
  // Return partial snippet with updated counters
  String html =
    "<p>Total images: " + String(totalImages) +
    "<br/>Classified true: " + String(totalTrueClassified) + "</p>";
  server.send(200, "text/html", html);
}

// --------------------------------------------------------------------------
// Update All button
// --------------------------------------------------------------------------
void handleUpdateAll() {
  // Build HTML snippet that contains OOB updates for both divs.
  // (1) The Image snippet
  String imageHtml;
  if (!lastImageBuf || lastImageLen == 0) {
    imageHtml = "<p><em>No image yet</em></p>";
  } else {
    imageHtml = "<img src=\"/latest.jpg?t=" + String(millis()) + "\" width=\"320\"/>";
  }

  // (2) The Classification + Counters snippet
  String classificationHtml =
    "<strong>classification_result = "
    + String(lastClassificationResult ? "true" : "false")
    + "</strong><br/>"
    "Total images: " + String(totalImages)
    + " | Classified true: " + String(totalTrueClassified);

  // Build a single HTML response with two <div> blocks that each have
  // "hx-swap-oob" so they replace the existing #imageDiv and #classificationDiv.
  // If you want counters in a separate <div id="resetMsg">, do so similarly.
  String html;
  html += "<div id=\"imageDiv\" hx-swap-oob=\"true\">" + imageHtml + "</div>";
  html += "<div id=\"classificationDiv\" hx-swap-oob=\"true\">"
           + classificationHtml + "</div>";

  // If your counters are in #resetMsg, you can also do:
  // html += "<div id=\"resetMsg\" hx-swap-oob=\"true\">" + countersHtml + "</div>";

  server.send(200, "text/html", html);
}


// --------------------------------------------------------------------------
// Serve the main page (which uses HTMX to make partial updates)
// --------------------------------------------------------------------------
void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-CAM Classifier Debug</title>
  <script src="https://unpkg.com/htmx.org@1.9.2"></script>
</head>
<body>
  <h1>ESP32-CAM Classifier Debug</h1>

  <div>
    <button 
      hx-post="/capture" 
      hx-target="#imageDiv" 
      hx-swap="innerHTML">
      Capture new image
    </button>

    <button 
      hx-post="/classify" 
      hx-target="#classificationDiv" 
      hx-swap="innerHTML">
      Classify last image
    </button>

    <!-- New: single update button that calls /updateAll -->
    <button
      hx-get="/updateAll"
      hx-swap="none">
      Update All
    </button>
  </div>

  <hr/>

  <!-- Simple static divs for image + classification (no auto triggers) -->
  <div id="imageDiv">
    <p><em>No image yet</em></p>
  </div>

  <div id="classificationDiv">
    <p><em>No classification yet</em></p>
  </div>

  <hr/>
  <h3>Configuration</h3>
  <div id="intervalMsg">
    <p>Current interval (ms): <strong>)rawliteral" + String(classificationInterval) + R"rawliteral(</strong></p>
  </div>
  
  <div>
    <label>Set new interval (ms):</label>
    <input id="intervalInput" name="val" type="number" value=")rawliteral" + String(classificationInterval) + R"rawliteral(" />
    <button 
      hx-post="/setInterval"
      hx-include="#intervalInput"
      hx-target="#intervalMsg"
      hx-swap="innerHTML">
      Update Interval
    </button>
  </div>

  <hr/>
  <div>
    <label>Current Prompt:</label>
    <div id="promptMsg">
      <pre>)rawliteral" + userPrompt + R"rawliteral(</pre>
    </div>
    
    <textarea id="promptInput" name="text" rows="4" cols="40">)rawliteral" + userPrompt + R"rawliteral(</textarea>
    <br/>
    <button
      hx-post="/setPrompt"
      hx-include="#promptInput"
      hx-target="#promptMsg"
      hx-swap="innerHTML">
      Update Prompt
    </button>
  </div>
  <hr/>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", page);
}


// --------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Connect Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  setupCamera();

  // WebServer routes
  server.on("/",                     HTTP_GET,  handleRoot);
  server.on("/latest.jpg",           HTTP_GET,  handleLatestJPG);
  server.on("/capture",              HTTP_POST, handleCapture);
  server.on("/classify",             HTTP_POST, handleClassify);
  server.on("/updateAll",            HTTP_GET, handleUpdateAll);

  server.on("/setInterval",          HTTP_POST, handleSetInterval);
  server.on("/setPrompt",            HTTP_POST, handleSetPrompt);
  server.on("/resetCounters",        HTTP_POST, handleResetCounters);

  server.begin();
  Serial.println("HTTP server started");
}

// --------------------------------------------------------------------------
// Loop - also handle the "auto classification" if interval has passed
// --------------------------------------------------------------------------
void loop() {
  server.handleClient();

  static unsigned long prev = 0;
  unsigned long now = millis();
  if (now - prev > classificationInterval) {
    prev = now;

    Serial.println("\n=== Auto-capture and classification ===");
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return;
    }

    if (lastImageBuf) {
      free(lastImageBuf);
      lastImageBuf = nullptr;
    }
    lastImageBuf = (uint8_t*) malloc(fb->len);
    if (lastImageBuf) {
      memcpy(lastImageBuf, fb->buf, fb->len);
      lastImageLen = fb->len;
    } else {
      lastImageLen = 0;
      Serial.println("Failed to allocate memory for auto-capture");
    }
    esp_camera_fb_return(fb);

    // Now do classification
    if (lastImageBuf && lastImageLen > 0) {
      camera_fb_t fb2;
      fb2.buf    = lastImageBuf;
      fb2.len    = lastImageLen;
      fb2.width  = 0;
      fb2.height = 0;
      fb2.format = PIXFORMAT_JPEG;

      String fileUri = uploadImage(&fb2);
      bool result = performClassification(fileUri);

      totalImages++;
      if (result) {
        totalTrueClassified++;
        triggerAlarm();
      }
      lastClassificationResult = result;
    }
  }
}
