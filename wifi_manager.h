#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

// Include the configuration file first
#include "config.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <LittleFS.h>

// AP mode configuration
#ifdef CAMERA_MODEL_XIAO_ESP32S3
#define AP_SSID "XIAO_ESP32S3_SENSE"
#elif defined(CAMERA_MODEL_AI_THINKER)
#define AP_SSID "AI_THINKER_CAM"
#elif defined(CAMERA_MODEL_ESP_EYE)
#define AP_SSID "ESP_EYE_CAM"
#endif

#define AP_PASSWORD ""  // Empty string for open network or set a password

#define AP_IP IPAddress(192, 168, 4, 1)
#define AP_GATEWAY IPAddress(192, 168, 4, 1)
#define AP_SUBNET IPAddress(255, 255, 255, 0)

// DNS Server for captive portal
#define DNS_PORT 53

// Timeout configuration
#define CONNECTION_TIMEOUT 5000  // 5 seconds to attempt connection
#define SCAN_TIMEOUT 5000        // 5 seconds to scan networks

// Credentials file
#define WIFI_CREDENTIALS_FILE "/wifi_credentials.json"

// WiFi credential structure
struct WifiCredential {
  String ssid;
  String password;

  WifiCredential(String s, String p)
    : ssid(s), password(p) {}
};

class WifiManager {
private:
  std::vector<WifiCredential> credentials;
  DNSServer dnsServer;
  bool apMode;
  unsigned long lastScanTime;
  std::vector<String> networkList;
  bool scanComplete;

  // Load credentials from file
  bool loadCredentials() {
    credentials.clear();

    if (!LittleFS.exists(WIFI_CREDENTIALS_FILE)) {
      Serial.println("\t⚠ No WiFi credentials file found");
      return false;
    }

    File file = LittleFS.open(WIFI_CREDENTIALS_FILE, "r");
    if (!file) {
      Serial.println("\t❌ Failed to open WiFi credentials file");
      return false;
    }

    // Calculate file size for JsonDocument
    size_t size = file.size();
    if (size > 1024) {
      Serial.println("\t❌ Credentials file too large");
      file.close();
      return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
      Serial.print("\t❌ Failed to parse JSON: ");
      Serial.println(error.c_str());
      return false;
    }

    if (!doc.is<JsonArray>()) {
      Serial.println("\t❌ Invalid JSON format (expected array)");
      return false;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject item : array) {
      if (item["wifi_ssid"].is<String>() && item["wifi_password"].is<String>()) {
        credentials.push_back(WifiCredential(
          item["wifi_ssid"].as<String>(),
          item["wifi_password"].as<String>()));
      }
    }

    Serial.print("\t✓ Loaded ");
    Serial.print(credentials.size());
    Serial.println(" WiFi networks from storage");
    return true;
  }

  // Save credentials to file
  bool saveCredentials() {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (const auto &cred : credentials) {
      JsonObject obj = array.add<JsonObject>();
      obj["wifi_ssid"] = cred.ssid;
      obj["wifi_password"] = cred.password;
    }

    File file = LittleFS.open(WIFI_CREDENTIALS_FILE, "w");
    if (!file) {
      Serial.println("\t❌ Failed to open WiFi credentials file for writing");
      return false;
    }

    if (serializeJson(doc, file) == 0) {
      Serial.println("\t❌ Failed to write credentials to file");
      file.close();
      return false;
    }

    file.close();
    Serial.println("\t✓ WiFi credentials saved successfully");
    return true;
  }

public:
  WifiManager() {
    apMode = false;
    lastScanTime = 0;
    scanComplete = false;
  }

  // Initialize WiFi Manager
  void begin() {
    Serial.println("\n3. WiFi Manager Initialization:");
    WiFi.mode(WIFI_STA);
    loadCredentials();
  }

  // Try to connect to saved networks
  bool connectToSavedNetworks() {
    if (credentials.empty()) {
      Serial.println("\t⚠ No saved WiFi networks found");
      return false;
    }

    int attempt = 0;
    for (const auto &cred : credentials) {
      attempt++;
      Serial.print("\t* Attempting connection to: ");
      Serial.print(cred.ssid);
      Serial.print(" (");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(credentials.size());
      Serial.println(")");
      Serial.print("\t");

      WiFi.begin(cred.ssid.c_str(), cred.password.c_str());

      unsigned long startTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startTime < CONNECTION_TIMEOUT) {
        delay(500);
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n\t✓ Connected!");
        printNetworkInfo();
        return true;
      } else {
        Serial.println("\n\t✗ Failed!");
        WiFi.disconnect();
      }
    }

    Serial.println("\t❌ Could not connect to any saved network");
    return false;
  }

  // Start AP mode with captive portal
  void startAPMode() {
    Serial.println("\tStarting AP Mode for configuration");

    // Free some memory first
    WiFi.disconnect(true);
    delay(1000);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    Serial.print("\t✓ AP started with SSID: ");
    Serial.println(AP_SSID);
    Serial.print("\t✓ IP Address: ");
    Serial.println(WiFi.softAPIP());

    // Give some time for AP to initialize fully before starting DNS
    delay(1000);

    // Start DNS server for captive portal
    dnsServer.start(DNS_PORT, "*", AP_IP);

    apMode = true;
  }

  void setAPMode(bool mode) {
    apMode = mode;
  }

  // Add new credentials and save
  bool addCredentials(const String &ssid, const String &password) {
    // Check if this network already exists
    for (auto &cred : credentials) {
      if (cred.ssid == ssid) {
        // Update password if it's the same SSID
        cred.password = password;
        return saveCredentials();
      }
    }

    // Add new credentials at the beginning for priority
    credentials.insert(credentials.begin(), WifiCredential(ssid, password));

    // Limit the number of saved networks if needed
    if (credentials.size() > 10) {
      credentials.pop_back();
    }

    return saveCredentials();
  }

  // Remove credentials
  bool removeCredentials(const String &ssid) {
    for (auto it = credentials.begin(); it != credentials.end(); ++it) {
      if (it->ssid == ssid) {
        credentials.erase(it);
        return saveCredentials();
      }
    }
    return false;
  }

  // Scan for available networks
  void scanNetworks() {
    if (millis() - lastScanTime < SCAN_TIMEOUT) {
      return;  // Don't scan too frequently
    }

    networkList.clear();
    scanComplete = false;

    Serial.println("\tScanning for WiFi networks...");
    int numNetworks = WiFi.scanNetworks();

    if (numNetworks == 0) {
      Serial.println("\t⚠ No networks found");
    } else {
      Serial.print("\t✓ Found ");
      Serial.print(numNetworks);
      Serial.println(" networks");

      // Sort networks by RSSI (signal strength)
      struct NetworkInfo {
        String ssid;
        int rssi;

        NetworkInfo(String s, int r)
          : ssid(s), rssi(r) {}

        bool operator<(const NetworkInfo &other) const {
          return rssi > other.rssi;  // Sort by descending signal strength
        }
      };

      std::vector<NetworkInfo> networks;

      for (int i = 0; i < numNetworks; i++) {
        // Only add networks with an SSID
        if (WiFi.SSID(i).length() > 0) {
          networks.push_back(NetworkInfo(WiFi.SSID(i), WiFi.RSSI(i)));
        }
      }

      // Sort by signal strength
      std::sort(networks.begin(), networks.end());

      // Add to network list, avoid duplicates
      for (const auto &net : networks) {
        bool isDuplicate = false;
        for (const auto &existingNet : networkList) {
          if (existingNet == net.ssid) {
            isDuplicate = true;
            break;
          }
        }

        if (!isDuplicate) {
          networkList.push_back(net.ssid);
          Serial.print("   • ");
          Serial.print(net.ssid);
          Serial.print(" (");
          Serial.print(net.rssi);
          Serial.println(" dBm)");
        }
      }
    }

    WiFi.scanDelete();  // Free memory used by scan
    lastScanTime = millis();
    scanComplete = true;
  }

  // Get scan results as JSON
  String getNetworkListJson() {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (const auto &ssid : networkList) {
      array.add(ssid);
    }

    String result;
    serializeJson(doc, result);
    return result;
  }

  // Check if in AP mode
  bool isAPMode() const {
    return apMode;
  }

  // Process DNS requests for captive portal
  void processDNS() {
    if (apMode) {
      dnsServer.processNextRequest();
    }
  }

  // Connect to a specific network
  bool connectToNetwork(const String &ssid, const String &password) {
    Serial.print("\tConnecting to: ");
    Serial.println(ssid);
    Serial.print("\t");

    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < CONNECTION_TIMEOUT) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\t✓ Connected!");

      // Add to saved networks if successful
      addCredentials(ssid, password);

      // Switch to station mode
      apMode = false;

      // Stop DNS server
      dnsServer.stop();

      printNetworkInfo();
      return true;
    } else {
      Serial.println("\t✗ Failed!");
      return false;
    }
  }

  // Print network information
  void printNetworkInfo() {
    Serial.println("\n\tNetwork Info:");
    Serial.println("\t------------");
    Serial.printf("\t⤷ IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("\t⤷ Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("\t⤷ Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("\t⤷ DNS: %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("\t⤷ MAC Address: %s\n", WiFi.macAddress().c_str());

    Serial.println("\n\tSignal Info:");
    Serial.println("\t-----------");
    Serial.printf("\t⤷ RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("\t⤷ Channel: %ld\n", WiFi.channel());

    Serial.println("\n\tConnection Info:");
    Serial.println("\t---------------");
    Serial.printf("\t⤷ SSID: %s\n", WiFi.SSID().c_str());
  }

  // Get scan status
  bool isScanComplete() const {
    return scanComplete;
  }
};

#endif  // WIFI_MANAGER_H