#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#define CONFIG_LITTLEFS_CACHE_SIZE 512

#include "wifi_manager.h"
#include "camera_init.h"

#ifdef CAMERA_MODEL_XIAO_ESP32S3
// No brownout includes needed
#elif defined(CAMERA_MODEL_AI_THINKER)
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif


AsyncWebServer server(80);  // Single server instance
WifiManager wifiManager;    // Create WiFi manager instance

bool isStreamActive = true;  // Flag to track Streaming State status
bool isConnecting = false;   // Flag to track WiFi connection status

String lastConnectionSSID = "";
String lastConnectionPassword = "";

// FreeRTOS task Used for Resetting over serial
// ** (This happens completely independently of your main loop)
TaskHandle_t serialMonitorTaskHandle;


// ======== Non-blocking MJPEG Stream ========
void handleMjpeg(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "multipart/x-mixed-replace; boundary=frame",
    [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      if (!isStreamActive) {
        return 0;  // Return 0 to stop streaming
      }

      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) return 0;

      size_t jpgLen = snprintf((char *)buffer, 100,
                               "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                               fb->len);

      if (jpgLen + fb->len > maxLen) {
        esp_camera_fb_return(fb);
        return 0;
      }

      memcpy(buffer + jpgLen, fb->buf, fb->len);
      esp_camera_fb_return(fb);
      return jpgLen + fb->len;
    });
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}


// Image saving
void handleCapture(AsyncWebServerRequest *request) {
  Serial.println("Received Save frame request ...");
  static unsigned long lastCapture = 0;
  unsigned long now = millis();

  if (now - lastCapture < 1000) {
    request->send(429, "text/plain", "Too many requests");
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    request->send(503, "text/plain", "Camera busy");
    return;
  }

  request->send(200, "image/jpeg", fb->buf, fb->len);
  esp_camera_fb_return(fb);
  lastCapture = now;
}


// Camera init with verbose output
void initCamera() {
  Serial.println("\n1. Checking Camera Status:");
  Serial.print("\t* Initializing camera... ");

  if (setupCamera()) {
    Serial.println("\t✓ Success");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
      Serial.println("\n\tCamera Details:");
      Serial.println("\t--------------");
      Serial.printf("\tResolution: %dx%d\n", sensor->status.framesize, sensor->status.framesize);
      Serial.printf("\tQuality: %d\n", sensor->status.quality);
      Serial.printf("\tBrightness: %d\n", sensor->status.brightness);
      Serial.printf("\tContrast: %d\n", sensor->status.contrast);
      Serial.printf("\tSaturation: %d\n", sensor->status.saturation);
      Serial.printf("\tSpecial Effect: %d\n", sensor->status.special_effect);
      Serial.printf("\tVertical Flip: %s\n", sensor->status.vflip ? "Yes" : "No");
      Serial.printf("\tHorizontal Mirror: %s\n", sensor->status.hmirror ? "Yes" : "No");
    }
    if (psramFound()) {
      Serial.println("\n\tMemory Info:");
      Serial.println("\t-----------");
      Serial.println("\tPSRAM: Available ✓");
      Serial.printf("\tFree PSRAM: %lu bytes\n", ESP.getFreePsram());
      Serial.printf("\tTotal PSRAM: %lu bytes\n", ESP.getPsramSize());
    } else {
      Serial.println("\n\t⚠ WARNING: No PSRAM detected");
      Serial.println("\tCamera will operate with limited buffer size");
    }
  } else {
    Serial.println("✗ Failed");
    Serial.println("\t❌ Fatal Error: Camera initialization failed");
    Serial.println("\tPlease check camera connection and pins");
    return;
  }

  Serial.println();
}


void initLittleFS() {
  Serial.println("\n2. Checking LittleFS Status:");
  Serial.println("\t* Mounting LittleFS... ");

  if (LittleFS.begin(false)) {
    Serial.println("\t✓ Mounted successfully (No formatting needed)");
  } else {
    Serial.println("\t✗ Mount failed");
    Serial.print("Attempting to format... ");

    if (LittleFS.format()) {
      Serial.println("\t✓ Format successful");
      Serial.print("Trying to mount again... ");

      if (LittleFS.begin()) {
        Serial.println("\t✓ Mounted successfully");
        Serial.println("\t⚠ WARNING: File system is empty!");
        Serial.println("⚠ Please upload files using ESP32 LittleFS Data Upload");
        Serial.println("⚠ Do not forget to close the serail monitor before");
        Serial.println("⚠ Then reset the device.");
      } else {
        Serial.println("✗ Mount failed after format");
        Serial.println("\t❌ Fatal Error: Storage unavailable");
        return;
      }
    } else {
      Serial.println("✗ Format failed");
      Serial.println("\t❌ Fatal Error: Unable to initialize storage");
      return;
    }
  }
  // Print LittleFS info for ESP32
  Serial.println("\n\tStorage Info:");
  Serial.println("\t------------");
  Serial.printf("\tTotal space: %u KB\n", LittleFS.totalBytes() / 1024);
  Serial.printf("\tUsed space: %u KB\n", LittleFS.usedBytes() / 1024);
  Serial.printf("\tFree space: %u KB\n", (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);

  // List all files
  Serial.println("\n\tFiles in storage:");
  Serial.println("\t---------------");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fileName = file.name();
    size_t fileSize = file.size();
    Serial.printf("\t• %-20s %8u bytes\n", fileName.c_str(), fileSize);
    file = root.openNextFile();
  }

  Serial.println();
}


void setupWIFIstn() {
  // Initialize the WiFi manager
  wifiManager.begin();

  // Try to connect to saved networks first
  if (!wifiManager.connectToSavedNetworks()) {
    // If no saved networks or connection fails, start AP mode
    wifiManager.startAPMode();
  }
}


void setup() {
#ifdef CAMERA_MODEL_XIAO_ESP32S3
  // Skip brownout and pin settings
#elif defined(CAMERA_MODEL_AI_THINKER)
  // Brownout prevention
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  // Camera power pin stabilization (AI Thinker specific)
  pinMode(12, OUTPUT);  // ESP32-CAM Flash LED pin
  digitalWrite(12, LOW);
  pinMode(10, OUTPUT);  // ESP32-CAM Flash LED pin
  digitalWrite(10, LOW);
#endif

  Serial.begin(115200);

  // Some small delay to wait for serial to begin
  Serial.println("\nWaiting 5 secs ...\n");
  delay(5000);

  Serial.println("\n___ ESP32-CAM-WEB-SERVER - (edgeImpulse tool)___");

  // 0. Create a task dedicated to monitoring serial input
  xTaskCreate(
    serialMonitorTask,        // Function to implement the task
    "SerialMonitorTask",      // Name of the task
    2048,                     // Stack size in words
    NULL,                     // Task input parameter
    1,                        // Priority of the task
    &serialMonitorTaskHandle  // Task handle
  );

  // 1. Cam init
  initCamera();
  // 2. LittleFS init
  initLittleFS();
  // 3. Connect to Wi Fi
  setupWIFIstn();

  // 4. Configure AsyncWebServer Routes
  // Static files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
    Serial.println("\tClient has tried to access ...");
  });

  server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/styles.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/script.js", "application/javascript");
  });
  // server.on("/ei-labeling-guide.png", HTTP_GET, [](AsyncWebServerRequest *request) {
  //   request->send(LittleFS, "/ei-labeling-guide.png", "image/png");
  // });

  // WiFi Portal files
  server.on("/wifi_portal.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/wifi_portal.html", "text/html");
  });

  server.on("/wifi_portal.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/wifi_portal.css", "text/css");
  });

  server.on("/wifi_portal.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/wifi_portal.js", "application/javascript");
  });

  // Stream endpoints
  server.on("/toggleStream", HTTP_POST, [](AsyncWebServerRequest *request) {
    isStreamActive = !isStreamActive;
    Serial.printf("\tStream state: %s\n", isStreamActive ? "Active" : "Paused");
    request->send(200, "text/plain", isStreamActive ? "streaming" : "paused");
  });

  server.on("/stream", HTTP_GET, handleMjpeg);

  // Capture endpoint
  server.on("/capture", HTTP_GET, handleCapture);

  // Clear endpoint: Both GET & POST
  server.on("/clear", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("\tReceived Clear all GET request ...");
    request->send(200, "text/plain", "Images cleared (GET)");
  });
  server.on("/clear", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("\tReceived Clear all POST request ...");
    request->send(200, "text/plain", "Images cleared (POST)");
  });

  server.on("/saveConfig", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("config", true)) {
      String config = request->getParam("config", true)->value();
      File file = LittleFS.open("/ei_config.json", "w");
      if (file) {
        file.print(config);
        file.close();
        Serial.println("\tConfiguration saved to LittleFS ...");
        request->send(200, "text/plain", "Configuration saved");
      } else {
        Serial.println("\tFailed to save configuration to LittleFS ...");
        request->send(500, "text/plain", "Failed to save configuration");
      }
    } else {
      request->send(400, "text/plain", "No configuration data");
    }
  });

  server.on("/loadConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/ei_config.json")) {
      request->send(LittleFS, "/ei_config.json", "application/json");
      Serial.println("  Configuration loaded from LittleFS ...");
    } else {
      Serial.println("\tNo configuration found in LittleFS ...");
      request->send(404, "text/plain", "No configuration found");
    }
  });

  // WiFi Manager API endpoints
  server.on("/wifi/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["apMode"] = wifiManager.isAPMode();
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    wifiManager.scanNetworks();
    request->send(200, "text/plain", "Scan started");
  });

  server.on("/wifi/networks", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (wifiManager.isScanComplete()) {
      request->send(200, "application/json", wifiManager.getNetworkListJson());
    } else {
      request->send(202, "text/plain", "Scan in progress");
    }
  });

  server.on("/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
      lastConnectionSSID = request->getParam("ssid", true)->value();
      lastConnectionPassword = request->getParam("password", true)->value();

      // Start connection process (non-blocking)
      isConnecting = true;
      // We need to respond to the client before attempting connection
      request->send(200, "text/plain", "Connection attempt started");
      // Connection attempt will be handled in the loop() function
    } else {
      request->send(400, "text/plain", "Missing parameters");
    }
  });

  server.on("/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["connected"] = (WiFi.status() == WL_CONNECTED);
    doc["connecting"] = isConnecting;
    if (WiFi.status() == WL_CONNECTED) {
      doc["ip"] = WiFi.localIP().toString();
      doc["ssid"] = WiFi.SSID();
    }
    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
  });

  server.on("/wifi/stopAP", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Respond first, then stop AP mode
    request->send(200, "text/plain", "Stopping AP mode");
    Serial.println("  Exiting AP MODE ...");
    // Schedule AP mode stop after response is sent
    WiFi.softAPdisconnect(true);
    wifiManager.setAPMode(false);
  });

  server.begin();

  Serial.println("Async HTTP server started on port 80\n");
  if (wifiManager.isAPMode()) {
    Serial.printf("👉🏼 Open http://%s:80 from a browser of a computer connected to WiFi SSID: %s\n",
                  AP_IP.toString().c_str(),
                  WiFi.SSID().c_str());
  } else {
    Serial.printf("👉🏼 Open http://%s:80 from a browser of a computer connected to WiFi SSID: %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.SSID().c_str());
  }
}


void loop() {
  // Monitor HEAP and PSRAM USAGE and apply a more aggressive restart control
  // Note: More easy would be if ESP.getFreeHeap() < 60000
  if (ESP.getFreeHeap() < 20000 || ESP.getFreePsram() < 10000) {
    Serial.printf("\tFree PSRAM: %lu bytes\n", ESP.getFreePsram());
    Serial.printf("\tFree Heap: %lu bytes\n\n", ESP.getFreeHeap());
    Serial.println("\tLow memory: Restarting\n");
    ESP.restart();
  }

  // Handle WiFi connection requests (non-blocking)
  if (isConnecting) {
    static String pendingSSID;
    static String pendingPassword;
    static unsigned long connectionStartTime = 0;
    static bool connectionInitiated = false;

    if (!connectionInitiated) {
      // Store the pending connection parameters
      pendingSSID = lastConnectionSSID;  // Use global variables set in the handler
      pendingPassword = lastConnectionPassword;
      connectionStartTime = millis();
      connectionInitiated = true;

      // Attempt connection
      bool result = wifiManager.connectToNetwork(pendingSSID, pendingPassword);

      if (result) {
        Serial.println("\t👍 Connection successful!");
      } else {
        Serial.println("\t❌ Connection failed!");
      }

      isConnecting = false;
      connectionInitiated = false;
    } else if (millis() - connectionStartTime > 15000) {
      // Timeout after 15 seconds
      isConnecting = false;
      connectionInitiated = false;
      Serial.println("\t ... Connection attempt timed out");
    }
  }

  // Process DNS in AP mode - needs to run every loop iteration, not just during connection
  if (wifiManager.isAPMode()) {
    wifiManager.processDNS();
  }

  delay(100);
}


// This task runs independently of the main loop
void serialMonitorTask(void *parameter) {
  for (;;) {  // Infinite loop
    if (Serial.available() > 0) {
      char incomingByte = Serial.read();
      if (incomingByte == 'r') {  // 'r' for reset
        Serial.println("\nRestarting ESP32...\n");
        Serial.println("\tBoot messages:\n");
        delay(1000);
        ESP.restart();
      }
    }
    // Small delay to prevent this task from consuming too much CPU
    vTaskDelay(10 / portTICK_PERIOD_MS);  // 10ms delay
  }
}