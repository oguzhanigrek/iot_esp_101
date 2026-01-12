/*
 * iot_esp_101 Sistemi - ESP32 Firmware
 * Deneyap Kart 1A
 *
 * WiFi Provisioning (Captive Portal) ile konfigürasyon
 * - AP modu ile başlar
 * - Kullanıcı WiFi ağlarını tarar ve seçer
 * - Ayarlar NVS'e kaydedilir
 * - Yeniden başlatmada otomatik bağlanır
 */

#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

// ============================================
// GLOBAL OBJECTS
// ============================================
WebServer server(WEB_SERVER_PORT);
DNSServer dnsServer;
Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============================================
// SYSTEM STATE
// ============================================
enum SystemMode {
  MODE_SETUP,  // AP mode - configuration portal
  MODE_RUNNING // Station mode - normal operation
};

struct SystemState {
  SystemMode mode;
  bool wifiConnected;
  bool ntpSynced;
  bool mqttConnected;
  unsigned long uptime;
  int freeHeap;
  int rssi;
  String savedSSID;
  String savedPassword;
  String mqttHost;
  int mqttPort;

  // Extended Configuration
  String deviceId;
  int readInterval;       // Sensör okuma aralığı (saniye)
  uint8_t sensorsEnabled; // Bit flags: NEM|SICAKLIK|UV|YAGMUR
  bool sleepEnabled;
  int sleepMinutes;
  int alarmNemMin;
  int alarmNemMax;
  int alarmTempMax;
  String ntpServer;
  int timezone;   // GMT offset in hours
  int debugLevel; // 0=Off, 1=Error, 2=Info, 3=Verbose
  bool ledEnabled;
} state;

// WiFi scan results
struct WiFiNetwork {
  String ssid;
  int rssi;
  bool secure;
};
std::vector<WiFiNetwork> scannedNetworks;
bool scanInProgress = false;

// ============================================
// FORWARD DECLARATIONS
// ============================================
void loadConfiguration();
void saveConfiguration();
bool connectToWiFi();
void startAPMode();
void startStationMode();
void setupWebServer();
void setupNTP();
void processSerialCommands();
void handleSerialCommand(String cmd);
void setupMQTT();
void maintainMQTT();
void publishStatus();
void publishSensors();
void mqttCallback(char *topic, byte *payload, unsigned int length);

// Web Handlers - Setup Mode
void handleRoot();
void handleScan();
void handleConnect();
void handleStatus();
void handleSaveConfig();
void handleReset();
void handleNotFound();

// Web Handlers - Running Mode
void handleDashboard();
void handleSystemInfo();

// Helpers
String getFormattedTime();
String getFormattedDate();
String getUptimeString();
String generateSetupHTML();
String generateDashboardHTML();
int getSignalQuality(int rssi);

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n");
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║    iot_esp_101 Sistemi - Deneyap Kart   ║");
  Serial.println("║           Firmware v" FIRMWARE_VERSION "               ║");
  Serial.println("╚══════════════════════════════════════════╝\n");

  // NVS'den konfigürasyonu yükle
  loadConfiguration();

  // Kayıtlı WiFi varsa bağlanmayı dene
  if (state.savedSSID.length() > 0) {
    Serial.println("[BOOT] Kayıtlı WiFi bulundu: " + state.savedSSID);

    if (connectToWiFi()) {
      startStationMode();
    } else {
      Serial.println(
          "[BOOT] WiFi bağlantısı başarısız, kurulum moduna geçiliyor...");
      startAPMode();
    }
  } else {
    Serial.println("[BOOT] Kayıtlı WiFi yok, kurulum moduna geçiliyor...");
    startAPMode();
  }

  // Web sunucuyu başlat
  setupWebServer();

  // MQTT başlat
  setupMQTT();

  Serial.println("\n[READY] Sistem hazır!");
}

// ============================================
// LOOP
// ============================================
void loop() {
  // DNS server (sadece AP modunda)
  if (state.mode == MODE_SETUP) {
    dnsServer.processNextRequest();
  }

  // Web sunucu
  server.handleClient();

  // Serial komutları işle
  processSerialCommands();

  // Sistem durumunu güncelle
  state.uptime = millis();
  state.freeHeap = ESP.getFreeHeap();

  if (state.mode == MODE_RUNNING) {
    state.rssi = WiFi.RSSI();
    state.wifiConnected = WiFi.status() == WL_CONNECTED;

    // MQTT yönetimi
    maintainMQTT();

    // WiFi kopmuşsa yeniden bağlan
    static unsigned long lastReconnect = 0;
    if (!state.wifiConnected && millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      Serial.println("[WiFi] Bağlantı koptu, yeniden bağlanılıyor...");
      WiFi.reconnect();
    }
  }

  delay(10);
}

// ============================================
// SERIAL COMMAND PROCESSING
// ============================================
String serialBuffer = "";

void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
      // Buffer limit
      if (serialBuffer.length() > 256) {
        serialBuffer = "";
      }
    }
  }
}

void handleSerialCommand(String cmd) {
  cmd.trim();
  Serial.println("[CMD] Alındı: " + cmd);

  // STATUS - Durum bilgisi
  if (cmd == "STATUS" || cmd == "status") {
    Serial.println("=== DURUM ===");
    Serial.println("Device ID: " + state.deviceId);
    Serial.println("Mode: " +
                   String(state.mode == MODE_SETUP ? "SETUP" : "RUNNING"));
    Serial.println("WiFi SSID: " + state.savedSSID);
    Serial.println("WiFi Connected: " +
                   String(state.wifiConnected ? "Yes" : "No"));
    Serial.println("IP: " + (state.mode == MODE_SETUP
                                 ? WiFi.softAPIP().toString()
                                 : WiFi.localIP().toString()));
    Serial.println("MQTT Host: " + state.mqttHost);
    Serial.println("MQTT Port: " + String(state.mqttPort));
    Serial.println("Read Interval: " + String(state.readInterval) + "s");
    Serial.println("Sensors: " + String(state.sensorsEnabled, BIN));
    Serial.println("Sleep: " + String(state.sleepEnabled ? "On" : "Off") +
                   " (" + String(state.sleepMinutes) + "min)");
    Serial.println("Alarms: Nem " + String(state.alarmNemMin) + "-" +
                   String(state.alarmNemMax) + "%, Temp <" +
                   String(state.alarmTempMax) + "C");
    Serial.println("Debug Level: " + String(state.debugLevel));
    Serial.println("LED: " + String(state.ledEnabled ? "On" : "Off"));
    Serial.println("Uptime: " + getUptimeString());
    Serial.println("Free Heap: " + String(state.freeHeap) + " bytes");
    Serial.println("Firmware: " FIRMWARE_VERSION);
    Serial.println("=============");

    // Frontend için JSON çıktısı
    JsonDocument statusDoc;
    statusDoc["serial_type"] = "status";
    statusDoc["deviceId"] = state.deviceId;
    statusDoc["ip"] = WiFi.localIP().toString();
    statusDoc["version"] = FIRMWARE_VERSION;
    statusDoc["rssi"] = WiFi.RSSI();
    statusDoc["uptime"] = getUptimeString();

    JsonObject config = statusDoc["config"].to<JsonObject>();
    config["ssid"] = state.savedSSID;
    config["mqtt_host"] = state.mqttHost;
    config["mqtt_port"] = state.mqttPort;
    config["read_interval"] = state.readInterval;
    config["sleep_mode"] = state.sleepEnabled ? "Aktif" : "Deaktif";

    Serial.print("JSON_STATUS:");
    serializeJson(statusDoc, Serial);
    Serial.println();
  }

  // GET_CONFIG - Tüm ayarları JSON olarak al
  else if (cmd == "GET_CONFIG" || cmd == "get_config") {
    Serial.println("{");
    Serial.println("  \"deviceId\": \"" + state.deviceId + "\",");
    Serial.println("  \"firmware\": \"" FIRMWARE_VERSION "\",");
    Serial.println("  \"mode\": \"" +
                   String(state.mode == MODE_SETUP ? "setup" : "running") +
                   "\",");
    Serial.println("  \"wifi\": {");
    Serial.println("    \"ssid\": \"" + state.savedSSID + "\",");
    Serial.println("    \"connected\": " +
                   String(state.wifiConnected ? "true" : "false") + ",");
    Serial.println("    \"ip\": \"" +
                   (state.mode == MODE_SETUP ? WiFi.softAPIP().toString()
                                             : WiFi.localIP().toString()) +
                   "\"");
    Serial.println("  },");
    Serial.println("  \"mqtt\": { \"host\": \"" + state.mqttHost +
                   "\", \"port\": " + String(state.mqttPort) + " },");
    Serial.println("  \"readInterval\": " + String(state.readInterval) + ",");
    Serial.println("  \"sensors\": " + String(state.sensorsEnabled) + ",");
    Serial.println("  \"sleep\": { \"enabled\": " +
                   String(state.sleepEnabled ? "true" : "false") +
                   ", \"minutes\": " + String(state.sleepMinutes) + " },");
    Serial.println("  \"alarms\": { \"nemMin\": " + String(state.alarmNemMin) +
                   ", \"nemMax\": " + String(state.alarmNemMax) +
                   ", \"tempMax\": " + String(state.alarmTempMax) + " },");
    Serial.println("  \"ntp\": { \"server\": \"" + state.ntpServer +
                   "\", \"timezone\": " + String(state.timezone) + " },");
    Serial.println("  \"debugLevel\": " + String(state.debugLevel) + ",");
    Serial.println("  \"led\": " + String(state.ledEnabled ? "true" : "false"));
    Serial.println("}");
  }

  // RESTART - Cihazı yeniden başlat
  else if (cmd == "RESTART" || cmd == "restart" || cmd == "REBOOT" ||
           cmd == "reboot") {
    Serial.println("[CMD] Cihaz yeniden başlatılıyor...");
    delay(500);
    ESP.restart();
  }

  // RESET - Fabrika ayarlarına dön
  else if (cmd == "RESET" || cmd == "reset" || cmd == "FACTORY_RESET") {
    Serial.println("[CMD] Fabrika ayarlarına dönülüyor...");
    preferences.begin(NVS_NAMESPACE, false);
    preferences.clear();
    preferences.end();
    Serial.println("[CMD] Ayarlar silindi, yeniden başlatılıyor...");
    delay(500);
    ESP.restart();
  }

  // SCAN - WiFi ağlarını tara
  else if (cmd == "SCAN" || cmd == "scan") {
    Serial.println("[CMD] WiFi ağları taranıyor...");
    int n = WiFi.scanNetworks();
    Serial.println("=== BULUNAN AĞLAR (" + String(n) + ") ===");
    for (int i = 0; i < n; i++) {
      Serial.println(String(i + 1) + ". " + WiFi.SSID(i) + " (" +
                     String(WiFi.RSSI(i)) + " dBm)" +
                     (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : " 🔒"));
    }
    Serial.println("=========================");
    WiFi.scanDelete();
  }

  // SET_WIFI:ssid,password
  else if (cmd.startsWith("SET_WIFI:") || cmd.startsWith("set_wifi:")) {
    String params = cmd.substring(9);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      state.savedSSID = params.substring(0, commaIndex);
      state.savedPassword = params.substring(commaIndex + 1);
      saveConfiguration();
      Serial.println("[CMD] WiFi ayarları kaydedildi! SSID: " +
                     state.savedSSID);
    } else {
      Serial.println("[CMD] HATA: Format: SET_WIFI:ssid,password");
    }
  }

  // SET_MQTT:host,port
  else if (cmd.startsWith("SET_MQTT:") || cmd.startsWith("set_mqtt:")) {
    String params = cmd.substring(9);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      state.mqttHost = params.substring(0, commaIndex);
      state.mqttPort = params.substring(commaIndex + 1).toInt();
      if (state.mqttPort <= 0)
        state.mqttPort = 1883;
      saveConfiguration();
      Serial.println("[CMD] MQTT ayarları kaydedildi! " + state.mqttHost + ":" +
                     String(state.mqttPort));
    } else {
      Serial.println("[CMD] HATA: Format: SET_MQTT:host,port");
    }
  }

  // SET_DEVICE_ID:name
  else if (cmd.startsWith("SET_DEVICE_ID:") ||
           cmd.startsWith("set_device_id:")) {
    state.deviceId = cmd.substring(14);
    state.deviceId.trim();
    saveConfiguration();
    Serial.println("[CMD] Device ID kaydedildi: " + state.deviceId);
  }

  // SET_READ_INTERVAL:seconds
  else if (cmd.startsWith("SET_READ_INTERVAL:") ||
           cmd.startsWith("set_read_interval:")) {
    int val = cmd.substring(18).toInt();
    if (val >= 5 && val <= 3600) {
      state.readInterval = val;
      saveConfiguration();
      Serial.println("[CMD] Okuma aralığı: " + String(val) + " saniye");
    } else {
      Serial.println("[CMD] HATA: Değer 5-3600 arası olmalı");
    }
  }

  // SET_SENSORS:nem,sicaklik,uv,yagmur (1/0)
  else if (cmd.startsWith("SET_SENSORS:") || cmd.startsWith("set_sensors:")) {
    String params = cmd.substring(12);
    uint8_t sensors = 0;
    int idx = 0;
    for (int i = 0; i < 4 && idx < params.length(); i++) {
      if (idx < params.length() && params.charAt(idx) == '1') {
        sensors |= (1 << i);
      }
      idx += 2; // Skip comma
    }
    state.sensorsEnabled = sensors;
    saveConfiguration();
    Serial.println("[CMD] Sensörler: " + String(sensors, BIN) + " (N:" +
                   String(sensors & 1) + " S:" + String((sensors >> 1) & 1) +
                   " U:" + String((sensors >> 2) & 1) +
                   " Y:" + String((sensors >> 3) & 1) + ")");
  }

  // SET_SLEEP:enable,minutes (1/0,dakika)
  else if (cmd.startsWith("SET_SLEEP:") || cmd.startsWith("set_sleep:")) {
    String params = cmd.substring(10);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      state.sleepEnabled = params.substring(0, commaIndex) == "1";
      state.sleepMinutes = params.substring(commaIndex + 1).toInt();
      if (state.sleepMinutes < 1)
        state.sleepMinutes = 5;
      saveConfiguration();
      Serial.println(
          "[CMD] Sleep: " + String(state.sleepEnabled ? "Aktif" : "Pasif") +
          ", " + String(state.sleepMinutes) + " dakika");
    } else {
      Serial.println("[CMD] HATA: Format: SET_SLEEP:0/1,dakika");
    }
  }

  // SET_ALARMS:nem_min,nem_max,temp_max
  else if (cmd.startsWith("SET_ALARMS:") || cmd.startsWith("set_alarms:")) {
    String params = cmd.substring(11);
    int c1 = params.indexOf(',');
    int c2 = params.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      state.alarmNemMin = params.substring(0, c1).toInt();
      state.alarmNemMax = params.substring(c1 + 1, c2).toInt();
      state.alarmTempMax = params.substring(c2 + 1).toInt();
      saveConfiguration();
      Serial.println("[CMD] Alarmlar: Nem " + String(state.alarmNemMin) + "-" +
                     String(state.alarmNemMax) + "%, Temp <" +
                     String(state.alarmTempMax) + "C");
    } else {
      Serial.println("[CMD] HATA: Format: SET_ALARMS:nem_min,nem_max,temp_max");
    }
  }

  // SET_NTP:server,timezone
  else if (cmd.startsWith("SET_NTP:") || cmd.startsWith("set_ntp:")) {
    String params = cmd.substring(8);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      state.ntpServer = params.substring(0, commaIndex);
      state.timezone = params.substring(commaIndex + 1).toInt();
      saveConfiguration();
      Serial.println("[CMD] NTP: " + state.ntpServer + ", GMT" +
                     (state.timezone >= 0 ? "+" : "") + String(state.timezone));
    } else {
      Serial.println("[CMD] HATA: Format: SET_NTP:server,timezone");
    }
  }

  // SET_DEBUG:level (0-3)
  else if (cmd.startsWith("SET_DEBUG:") || cmd.startsWith("set_debug:")) {
    int val = cmd.substring(10).toInt();
    if (val >= 0 && val <= 3) {
      state.debugLevel = val;
      saveConfiguration();
      String levels[] = {"Off", "Error", "Info", "Verbose"};
      Serial.println("[CMD] Debug seviyesi: " + levels[val]);
    } else {
      Serial.println("[CMD] HATA: 0=Off, 1=Error, 2=Info, 3=Verbose");
    }
  }

  // SET_LED:0/1
  else if (cmd.startsWith("SET_LED:") || cmd.startsWith("set_led:")) {
    state.ledEnabled = cmd.substring(8) == "1";
    saveConfiguration();
    Serial.println("[CMD] LED: " +
                   String(state.ledEnabled ? "Açık" : "Kapalı"));
  }

  // HELP - Yardım
  else if (cmd == "HELP" || cmd == "help" || cmd == "?") {
    Serial.println("=== TEMEL KOMUTLAR ===");
    Serial.println("STATUS                  - Sistem durumu");
    Serial.println("GET_CONFIG              - JSON konfigürasyon");
    Serial.println("SCAN                    - WiFi tara");
    Serial.println("RESTART                 - Yeniden başlat");
    Serial.println("RESET                   - Fabrika ayarı");
    Serial.println("");
    Serial.println("=== AYAR KOMUTLARI ===");
    Serial.println("SET_DEVICE_ID:name      - Cihaz ID");
    Serial.println("SET_WIFI:ssid,pass      - WiFi ayarla");
    Serial.println("SET_MQTT:host,port      - MQTT ayarla");
    Serial.println("SET_READ_INTERVAL:sn    - Okuma aralığı (5-3600)");
    Serial.println("SET_SENSORS:n,s,u,y     - Sensörler (1/0)");
    Serial.println("SET_SLEEP:en,dk         - Uyku modu");
    Serial.println("SET_ALARMS:nmin,nmax,t  - Alarm eşikleri");
    Serial.println("SET_NTP:server,tz       - NTP ayarları");
    Serial.println("SET_DEBUG:0-3           - Debug seviyesi");
    Serial.println("SET_LED:0/1             - LED durumu");
    Serial.println("======================");
  }

  // Bilinmeyen komut
  else {
    Serial.println("[CMD] Bilinmeyen komut. 'HELP' yazın.");
  }
}

// ============================================
// CONFIGURATION MANAGEMENT
// ============================================
void loadConfiguration() {
  Serial.println("[NVS] Konfigürasyon yükleniyor...");

  preferences.begin(NVS_NAMESPACE, true); // readonly

  // Basic settings
  state.savedSSID = preferences.getString(NVS_KEY_WIFI_SSID, "");
  state.savedPassword = preferences.getString(NVS_KEY_WIFI_PASS, "");
  state.mqttHost = preferences.getString(NVS_KEY_MQTT_HOST, "");
  state.mqttPort = preferences.getInt(NVS_KEY_MQTT_PORT, MQTT_PORT);
  state.deviceId = preferences.getString(NVS_KEY_DEVICE_ID, DEVICE_ID);

  // Extended settings
  state.readInterval =
      preferences.getInt(NVS_KEY_READ_INTERVAL, DEFAULT_READ_INTERVAL);
  state.sensorsEnabled =
      preferences.getUChar(NVS_KEY_SENSORS_ENABLED, DEFAULT_SENSORS_ENABLED);
  state.sleepEnabled =
      preferences.getBool(NVS_KEY_SLEEP_ENABLED, DEFAULT_SLEEP_ENABLED);
  state.sleepMinutes =
      preferences.getInt(NVS_KEY_SLEEP_MINUTES, DEFAULT_SLEEP_MINUTES);
  state.alarmNemMin =
      preferences.getInt(NVS_KEY_ALARM_NEM_MIN, DEFAULT_ALARM_NEM_MIN);
  state.alarmNemMax =
      preferences.getInt(NVS_KEY_ALARM_NEM_MAX, DEFAULT_ALARM_NEM_MAX);
  state.alarmTempMax =
      preferences.getInt(NVS_KEY_ALARM_TEMP_MAX, DEFAULT_ALARM_TEMP_MAX);
  state.ntpServer = preferences.getString(NVS_KEY_NTP_SERVER, NTP_SERVER);
  state.timezone = preferences.getInt(NVS_KEY_TIMEZONE, 3); // GMT+3
  state.debugLevel =
      preferences.getInt(NVS_KEY_DEBUG_LEVEL, DEFAULT_DEBUG_LEVEL);
  state.ledEnabled =
      preferences.getBool(NVS_KEY_LED_ENABLED, DEFAULT_LED_ENABLED);

  preferences.end();

  Serial.println("[NVS] Device ID: " + state.deviceId);
  Serial.println("[NVS] SSID: " +
                 (state.savedSSID.length() > 0 ? state.savedSSID : "(boş)"));
  Serial.println("[NVS] MQTT: " +
                 (state.mqttHost.length() > 0 ? state.mqttHost : "(boş)"));
  Serial.println("[NVS] Read Interval: " + String(state.readInterval) + "s");
}

void saveConfiguration() {
  Serial.println("[NVS] Konfigürasyon kaydediliyor...");

  preferences.begin(NVS_NAMESPACE, false); // read-write

  // Basic settings
  preferences.putString(NVS_KEY_WIFI_SSID, state.savedSSID);
  preferences.putString(NVS_KEY_WIFI_PASS, state.savedPassword);
  preferences.putString(NVS_KEY_MQTT_HOST, state.mqttHost);
  preferences.putInt(NVS_KEY_MQTT_PORT, state.mqttPort);
  preferences.putString(NVS_KEY_DEVICE_ID, state.deviceId);
  preferences.putBool(NVS_KEY_CONFIGURED, true);

  // Extended settings
  preferences.putInt(NVS_KEY_READ_INTERVAL, state.readInterval);
  preferences.putUChar(NVS_KEY_SENSORS_ENABLED, state.sensorsEnabled);
  preferences.putBool(NVS_KEY_SLEEP_ENABLED, state.sleepEnabled);
  preferences.putInt(NVS_KEY_SLEEP_MINUTES, state.sleepMinutes);
  preferences.putInt(NVS_KEY_ALARM_NEM_MIN, state.alarmNemMin);
  preferences.putInt(NVS_KEY_ALARM_NEM_MAX, state.alarmNemMax);
  preferences.putInt(NVS_KEY_ALARM_TEMP_MAX, state.alarmTempMax);
  preferences.putString(NVS_KEY_NTP_SERVER, state.ntpServer);
  preferences.putInt(NVS_KEY_TIMEZONE, state.timezone);
  preferences.putInt(NVS_KEY_DEBUG_LEVEL, state.debugLevel);
  preferences.putBool(NVS_KEY_LED_ENABLED, state.ledEnabled);

  preferences.end();

  Serial.println("[NVS] Konfigürasyon kaydedildi!");
}

// ============================================
// WiFi MANAGEMENT
// ============================================
bool connectToWiFi() {
  Serial.println("[WiFi] Bağlanılıyor: " + state.savedSSID);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_ID);
  WiFi.begin(state.savedSSID.c_str(), state.savedPassword.c_str());

  unsigned long startTime = millis();
  int dots = 0;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots % 40 == 0)
      Serial.println();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    state.wifiConnected = true;
    state.rssi = WiFi.RSSI();
    Serial.println("[WiFi] Bağlantı başarılı!");
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("[WiFi] Bağlantı başarısız!");
  return false;
}

void startAPMode() {
  Serial.println("[AP] Access Point başlatılıyor...");

  state.mode = MODE_SETUP;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONNECTIONS);

  // DNS server - tüm istekleri buraya yönlendir (captive portal)
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  Serial.println("[AP] SSID: " AP_SSID);
  Serial.println("[AP] Şifre: " AP_PASSWORD);
  Serial.println("[AP] IP: " + WiFi.softAPIP().toString());
  Serial.println("[AP] Kurulum için http://" + WiFi.softAPIP().toString() +
                 " adresine gidin");
}

void startStationMode() {
  Serial.println("[STA] Station modu başlatılıyor...");

  state.mode = MODE_RUNNING;

  // NTP senkronizasyonu
  setupNTP();
}

// ============================================
// NTP TIME SYNC
// ============================================
void setupNTP() {
  Serial.println("[NTP] Saat senkronizasyonu (İstanbul UTC+3)...");

  // pool.ntp.org yerine tr.pool.ntp.org kullanarak yerelleştirme yapıyoruz
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT_OFFSET, "tr.pool.ntp.org",
             "pool.ntp.org");

  struct tm timeinfo;
  int retry = 0;

  while (!getLocalTime(&timeinfo) && retry < 10) {
    Serial.print(".");
    delay(1000);
    retry++;
  }
  Serial.println();

  if (retry < 10) {
    state.ntpSynced = true;
    Serial.println("[NTP] Senkronize: " + getFormattedDate() + " " +
                   getFormattedTime());
  } else {
    state.ntpSynced = false;
    Serial.println("[NTP] Senkronizasyon başarısız!");
  }
}

// ============================================
// WEB SERVER SETUP
// ============================================
void setupWebServer() {
  Serial.println("[WEB] Sunucu başlatılıyor...");

  // Common routes
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset", HTTP_POST, handleReset);

  if (state.mode == MODE_SETUP) {
    // Setup mode routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/connect", HTTP_POST, handleConnect);
    server.on("/save", HTTP_POST, handleSaveConfig);
    server.on("/generate_204", HTTP_GET, handleRoot); // Android captive portal
    server.on("/fwlink", HTTP_GET, handleRoot);       // Windows captive portal
  } else {
    // Running mode routes
    server.on("/", HTTP_GET, handleDashboard);
    server.on("/api/system", HTTP_GET, handleSystemInfo);
  }

  server.onNotFound(handleNotFound);
  server.enableCORS(true);
  server.begin();

  Serial.println("[WEB] Sunucu başlatıldı (Port: " + String(WEB_SERVER_PORT) +
                 ")");
}

// ============================================
// SETUP MODE - WEB HANDLERS
// ============================================
void handleRoot() { server.send(200, "text/html", generateSetupHTML()); }

void handleScan() {
  Serial.println("[WiFi] Ağlar taranıyor...");

  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();

  int n = WiFi.scanNetworks();
  Serial.println("[WiFi] " + String(n) + " ağ bulundu");

  for (int i = 0; i < n; i++) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    network["quality"] = getSignalQuality(WiFi.RSSI(i));
  }

  doc["count"] = n;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);

  WiFi.scanDelete();
}

void handleConnect() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"No data\"}");
    return;
  }

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));

  String ssid = doc["ssid"].as<String>();
  String password = doc["password"].as<String>();

  Serial.println("[WiFi] Bağlanılıyor: " + ssid);

  // Mevcut AP'yi kapat ve STA moduna geç
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  // Bağlantıyı bekle
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  JsonDocument response;

  if (WiFi.status() == WL_CONNECTED) {
    state.savedSSID = ssid;
    state.savedPassword = password;
    state.wifiConnected = true;

    response["success"] = true;
    response["message"] = "Bağlantı başarılı!";
    response["ip"] = WiFi.localIP().toString();

    Serial.println("[WiFi] Bağlandı! IP: " + WiFi.localIP().toString());
  } else {
    response["success"] = false;
    response["message"] = "Bağlantı başarısız! Şifreyi kontrol edin.";

    // AP moduna geri dön
    WiFi.mode(WIFI_AP);
    Serial.println("[WiFi] Bağlantı başarısız!");
  }

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

void handleSaveConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"No data\"}");
    return;
  }

  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));

  // MQTT ayarları
  if (doc.containsKey("mqttHost")) {
    state.mqttHost = doc["mqttHost"].as<String>();
  }
  if (doc.containsKey("mqttPort")) {
    state.mqttPort = doc["mqttPort"].as<int>();
  }

  // Konfigürasyonu kaydet
  saveConfiguration();

  // MQTT ayarlarını güncelle
  setupMQTT();

  JsonDocument response;
  response["success"] = true;
  response["message"] = "Ayarlar kaydedildi! Cihaz yeniden başlatılıyor...";

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  // Yeniden başlat
  delay(1000);
  ESP.restart();
}

void handleStatus() {
  JsonDocument doc;

  doc["mode"] = state.mode == MODE_SETUP ? "setup" : "running";
  doc["wifiConnected"] = state.wifiConnected;
  doc["ssid"] = state.savedSSID;
  doc["ip"] = state.mode == MODE_SETUP ? WiFi.softAPIP().toString()
                                       : WiFi.localIP().toString();
  doc["mqttHost"] = state.mqttHost;
  doc["mqttPort"] = state.mqttPort;
  doc["uptime"] = getUptimeString();
  doc["freeHeap"] = state.freeHeap;
  doc["firmware"] = FIRMWARE_VERSION;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleReset() {
  Serial.println("[SYS] Fabrika ayarlarına dönülüyor...");

  preferences.begin(NVS_NAMESPACE, false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json",
              "{\"success\":true,\"message\":\"Ayarlar silindi, yeniden "
              "başlatılıyor...\"}");

  delay(1000);
  ESP.restart();
}

// ============================================
// RUNNING MODE - WEB HANDLERS
// ============================================
void handleDashboard() {
  server.send(200, "text/html", generateDashboardHTML());
}

void handleSystemInfo() {
  JsonDocument doc;

  // Zaman
  doc["time"] = getFormattedTime();
  doc["date"] = getFormattedDate();
  doc["ntpSynced"] = state.ntpSynced;

  // Network
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["ssid"] = state.savedSSID;
  doc["rssi"] = WiFi.RSSI();
  doc["signalQuality"] = getSignalQuality(WiFi.RSSI());
  doc["wifiConnected"] = state.wifiConnected;

  // Device
  doc["deviceName"] = DEVICE_NAME;
  doc["deviceId"] = DEVICE_ID;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime"] = getUptimeString();
  doc["mode"] = "running";

  // System
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["cpuFreq"] = ESP.getCpuFreqMHz();
  doc["flashSize"] = ESP.getFlashChipSize() / (1024 * 1024);

  // MQTT
  doc["mqttHost"] = state.mqttHost;
  doc["mqttPort"] = state.mqttPort;
  doc["mqttConnected"] = state.mqttConnected;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  // Captive portal için tüm istekleri ana sayfaya yönlendir
  if (state.mode == MODE_SETUP) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404 - Sayfa Bulunamadi");
  }
}

// ============================================
// HTML GENERATORS
// ============================================
String generateSetupHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="tr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
    <title>iot_esp_101 - Kurulum</title>
    <style>
        :root {
            --bg: #0a0a1a;
            --card: #12122a;
            --primary: #00d4aa;
            --secondary: #7c3aed;
            --danger: #ef4444;
            --warning: #fbbf24;
            --text: #ffffff;
            --text-dim: #8888aa;
            --border: rgba(124, 58, 237, 0.3);
        }
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
            background: var(--bg);
            color: var(--text);
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 480px; margin: 0 auto; }
        
        header {
            text-align: center;
            padding: 30px 0 40px;
        }
        .logo { font-size: 4rem; margin-bottom: 15px; }
        h1 {
            font-size: 1.8rem;
            background: linear-gradient(135deg, var(--primary), var(--secondary));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .subtitle { color: var(--text-dim); margin-top: 8px; }
        
        .card {
            background: var(--card);
            border: 1px solid var(--border);
            border-radius: 16px;
            padding: 24px;
            margin-bottom: 20px;
        }
        .card-title {
            display: flex;
            align-items: center;
            gap: 10px;
            font-size: 1.1rem;
            margin-bottom: 20px;
            color: var(--primary);
        }
        
        .step-indicator {
            display: flex;
            justify-content: center;
            gap: 10px;
            margin-bottom: 30px;
        }
        .step {
            width: 40px;
            height: 40px;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            font-weight: 600;
            background: var(--card);
            border: 2px solid var(--border);
            color: var(--text-dim);
        }
        .step.active {
            background: var(--primary);
            border-color: var(--primary);
            color: var(--bg);
        }
        .step.completed {
            background: var(--secondary);
            border-color: var(--secondary);
            color: white;
        }
        
        .btn {
            width: 100%;
            padding: 14px 20px;
            border: none;
            border-radius: 10px;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
        }
        .btn-primary {
            background: linear-gradient(135deg, var(--primary), #00b894);
            color: var(--bg);
        }
        .btn-primary:hover { transform: translateY(-2px); box-shadow: 0 5px 20px rgba(0, 212, 170, 0.4); }
        .btn-primary:disabled { opacity: 0.5; cursor: not-allowed; transform: none; }
        
        .btn-secondary {
            background: transparent;
            border: 1px solid var(--border);
            color: var(--text);
        }
        
        .btn-danger {
            background: transparent;
            border: 1px solid var(--danger);
            color: var(--danger);
        }
        
        .network-list {
            max-height: 300px;
            overflow-y: auto;
            margin-bottom: 15px;
        }
        .network-item {
            display: flex;
            align-items: center;
            padding: 12px 15px;
            border: 1px solid var(--border);
            border-radius: 10px;
            margin-bottom: 8px;
            cursor: pointer;
            transition: all 0.2s;
        }
        .network-item:hover, .network-item.selected {
            border-color: var(--primary);
            background: rgba(0, 212, 170, 0.1);
        }
        .network-icon { font-size: 1.5rem; margin-right: 12px; }
        .network-info { flex: 1; }
        .network-name { font-weight: 500; }
        .network-signal { font-size: 0.85rem; color: var(--text-dim); }
        .signal-bars {
            display: flex;
            align-items: flex-end;
            gap: 2px;
            height: 16px;
        }
        .signal-bar {
            width: 4px;
            background: var(--text-dim);
            border-radius: 1px;
        }
        .signal-bar.active { background: var(--primary); }
        .signal-bar:nth-child(1) { height: 25%; }
        .signal-bar:nth-child(2) { height: 50%; }
        .signal-bar:nth-child(3) { height: 75%; }
        .signal-bar:nth-child(4) { height: 100%; }
        
        .form-group { margin-bottom: 15px; }
        .form-label {
            display: block;
            margin-bottom: 8px;
            color: var(--text-dim);
            font-size: 0.9rem;
        }
        .form-input {
            width: 100%;
            padding: 12px 15px;
            background: var(--bg);
            border: 1px solid var(--border);
            border-radius: 10px;
            color: var(--text);
            font-size: 1rem;
        }
        .form-input:focus {
            outline: none;
            border-color: var(--primary);
        }
        
        .password-wrapper {
            position: relative;
        }
        .password-toggle {
            position: absolute;
            right: 12px;
            top: 50%;
            transform: translateY(-50%);
            background: none;
            border: none;
            color: var(--text-dim);
            cursor: pointer;
            font-size: 1.2rem;
        }
        
        .alert {
            padding: 12px 15px;
            border-radius: 10px;
            margin-bottom: 15px;
            display: none;
        }
        .alert.success {
            background: rgba(0, 212, 170, 0.2);
            border: 1px solid var(--primary);
            color: var(--primary);
        }
        .alert.error {
            background: rgba(239, 68, 68, 0.2);
            border: 1px solid var(--danger);
            color: var(--danger);
        }
        .alert.show { display: block; }
        
        .spinner {
            width: 20px;
            height: 20px;
            border: 2px solid transparent;
            border-top-color: currentColor;
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }
        @keyframes spin { to { transform: rotate(360deg); } }
        
        .hidden { display: none !important; }
        
        .connection-success {
            text-align: center;
            padding: 30px 0;
        }
        .success-icon {
            font-size: 4rem;
            margin-bottom: 20px;
        }
        .success-ip {
            font-family: monospace;
            font-size: 1.3rem;
            color: var(--primary);
            background: rgba(0, 212, 170, 0.1);
            padding: 10px 20px;
            border-radius: 8px;
            margin: 15px 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div class="logo">🌱</div>
            <h1>iot_esp_101 Sistemi</h1>
            <p class="subtitle">Kurulum Sihirbazı</p>
        </header>
        
        <div class="step-indicator">
            <div class="step active" id="step1">1</div>
            <div class="step" id="step2">2</div>
        </div>
        
        <!-- Step 1: WiFi Selection -->
        <div id="wifiSection" class="card">
            <div class="card-title">
                <span>📡</span>
                <span>WiFi Ağı Seçin</span>
            </div>
            
            <div id="networkList" class="network-list">
                <p style="text-align: center; color: var(--text-dim); padding: 20px;">
                    Ağları taramak için butona tıklayın
                </p>
            </div>
            
            <button class="btn btn-primary" onclick="scanNetworks()" id="scanBtn">
                <span>🔍</span>
                <span>Ağları Tara</span>
            </button>
        </div>
        
        <!-- Step 2: Password Entry -->
        <div id="passwordSection" class="card hidden">
            <div class="card-title">
                <span>🔐</span>
                <span id="selectedNetworkName">WiFi Şifresi</span>
            </div>
            
            <div id="alertBox" class="alert"></div>
            
            <div class="form-group">
                <label class="form-label">Ağ Adı (SSID)</label>
                <input type="text" class="form-input" id="ssidInput" readonly>
            </div>
            
            <div class="form-group">
                <label class="form-label">Şifre</label>
                <div class="password-wrapper">
                    <input type="password" class="form-input" id="passwordInput" placeholder="WiFi şifrenizi girin">
                    <button class="password-toggle" onclick="togglePassword()">👁️</button>
                </div>
            </div>
            
            <button class="btn btn-primary" onclick="connectWiFi()" id="connectBtn" style="margin-bottom: 10px;">
                <span>📶</span>
                <span>Bağlan</span>
            </button>
            
            <button class="btn btn-secondary" onclick="goBack()">
                <span>←</span>
                <span>Geri Dön</span>
            </button>
        </div>
        
        <!-- Step 3: Finalize -->
        <div id="serverSection" class="card hidden">
            <div class="card-title">
                <span>✨</span>
                <span>Kurulum Tamamlanıyor</span>
            </div>
            
            <div class="connection-success">
                <div class="success-icon">🎉</div>
                <h3>WiFi Bağlantısı Başarılı!</h3>
                <p class="subtitle">Cihaz kaydediliyor ve yeniden başlatılıyor...</p>
                <div class="success-ip" id="deviceIP">0.0.0.0</div>
            </div>
            
            <div id="saveBtn" class="hidden"></div>
            <input type="hidden" id="mqttHostInput" value="">
            <input type="hidden" id="mqttPortInput" value="1883">
        </div>
        
        <!-- Settings Link -->
        <div style="text-align: center; margin-top: 20px;">
            <button class="btn btn-danger" onclick="factoryReset()" style="width: auto; padding: 10px 20px;">
                🔄 Fabrika Ayarlarına Dön
            </button>
        </div>
    </div>
    
    <script>
        let selectedNetwork = null;
        
        async function scanNetworks() {
            const btn = document.getElementById('scanBtn');
            const list = document.getElementById('networkList');
            
            btn.disabled = true;
            btn.innerHTML = '<div class="spinner"></div><span>Taranıyor...</span>';
            
            try {
                const res = await fetch('/scan');
                const data = await res.json();
                
                if (data.networks.length === 0) {
                    list.innerHTML = '<p style="text-align: center; color: var(--text-dim); padding: 20px;">Ağ bulunamadı</p>';
                } else {
                    list.innerHTML = data.networks.map(n => `
                        <div class="network-item" onclick="selectNetwork('${n.ssid}', ${n.secure})">
                            <div class="network-icon">${n.secure ? '🔒' : '📶'}</div>
                            <div class="network-info">
                                <div class="network-name">${n.ssid}</div>
                                <div class="network-signal">${n.rssi} dBm - %${n.quality} sinyal</div>
                            </div>
                            <div class="signal-bars">
                                ${[1,2,3,4].map(i => `<div class="signal-bar ${n.quality >= i*25 ? 'active' : ''}"></div>`).join('')}
                            </div>
                        </div>
                    `).join('');
                }
            } catch (e) {
                list.innerHTML = '<p style="text-align: center; color: var(--danger); padding: 20px;">Tarama hatası!</p>';
            }
            
            btn.disabled = false;
            btn.innerHTML = '<span>🔍</span><span>Tekrar Tara</span>';
        }
        
        function selectNetwork(ssid, secure) {
            selectedNetwork = { ssid, secure };
            document.getElementById('ssidInput').value = ssid;
            document.getElementById('selectedNetworkName').textContent = ssid;
            
            document.getElementById('wifiSection').classList.add('hidden');
            document.getElementById('passwordSection').classList.remove('hidden');
            
            document.getElementById('step1').classList.remove('active');
            document.getElementById('step1').classList.add('completed');
            document.getElementById('step2').classList.add('active');
            
            if (!secure) {
                document.getElementById('passwordInput').value = '';
                document.getElementById('passwordInput').placeholder = 'Açık ağ - şifre gerekmez';
            }
        }
        
        function goBack() {
            document.getElementById('wifiSection').classList.remove('hidden');
            document.getElementById('passwordSection').classList.add('hidden');
            
            document.getElementById('step1').classList.add('active');
            document.getElementById('step1').classList.remove('completed');
            document.getElementById('step2').classList.remove('active');
            
            hideAlert();
        }
        
        async function connectWiFi() {
            const btn = document.getElementById('connectBtn');
            const password = document.getElementById('passwordInput').value;
            
            btn.disabled = true;
            btn.innerHTML = '<div class="spinner"></div><span>Bağlanılıyor...</span>';
            hideAlert();
            
            try {
                const res = await fetch('/connect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        ssid: selectedNetwork.ssid,
                        password: password
                    })
                });
                
                const data = await res.json();
                
                if (data.success) {
                    showAlert('Bağlantı başarılı!', 'success');
                    document.getElementById('deviceIP').textContent = data.ip;
                    
                    setTimeout(() => {
                        document.getElementById('passwordSection').classList.add('hidden');
                        document.getElementById('serverSection').classList.remove('hidden');
                        
                        document.getElementById('step2').classList.remove('active');
                        document.getElementById('step2').classList.add('completed');
                        
                        // Otomatik kaydet ve başlat
                        saveConfig();
                    }, 1500);
                } else {
                    showAlert(data.message, 'error');
                }
            } catch (e) {
                showAlert('Bağlantı hatası!', 'error');
            }
            
            btn.disabled = false;
            btn.innerHTML = '<span>📶</span><span>Bağlan</span>';
        }
        
        async function saveConfig() {
            const btn = document.getElementById('saveBtn');
            const mqttHost = document.getElementById('mqttHostInput').value;
            const mqttPort = document.getElementById('mqttPortInput').value;
            
            btn.disabled = true;
            btn.innerHTML = '<div class="spinner"></div><span>Kaydediliyor...</span>';
            
            try {
                await fetch('/save', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        mqttHost: mqttHost,
                        mqttPort: parseInt(mqttPort)
                    })
                });
                
                alert('Ayarlar kaydedildi! Cihaz yeniden başlatılıyor...');
            } catch (e) {
                alert('Kayıt hatası!');
            }
        }
        
        function skipServer() {
            if (confirm('Sunucu ayarları olmadan devam etmek istiyor musunuz?')) {
                saveConfig();
            }
        }
        
        async function factoryReset() {
            if (confirm('Tüm ayarlar silinecek. Emin misiniz?')) {
                try {
                    await fetch('/reset', { method: 'POST' });
                    alert('Ayarlar silindi! Cihaz yeniden başlatılıyor...');
                } catch (e) {
                    alert('Hata!');
                }
            }
        }
        
        function togglePassword() {
            const input = document.getElementById('passwordInput');
            input.type = input.type === 'password' ? 'text' : 'password';
        }
        
        function showAlert(message, type) {
            const alert = document.getElementById('alertBox');
            alert.textContent = message;
            alert.className = 'alert ' + type + ' show';
        }
        
        function hideAlert() {
            document.getElementById('alertBox').className = 'alert';
        }
    </script>
</body>
</html>
)rawliteral";
}

// ============================================
// MQTT MANAGEMENT
// ============================================
void setupMQTT() {
  if (state.mqttHost.length() == 0)
    return;

  mqttClient.setServer(state.mqttHost.c_str(), state.mqttPort);
  mqttClient.setCallback(mqttCallback);
  Serial.println("[MQTT] Sunucu ayarlandı: " + state.mqttHost + ":" +
                 String(state.mqttPort));
}

void maintainMQTT() {
  if (state.mode != MODE_RUNNING || state.mqttHost.length() == 0 ||
      !state.wifiConnected)
    return;

  if (!mqttClient.connected()) {
    static unsigned long lastMqttRetry = 0;
    if (millis() - lastMqttRetry > 5000) {
      lastMqttRetry = millis();
      Serial.print("[MQTT] Bağlanılıyor...");

      String clientId = "Deneyap-" + state.deviceId;
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(" Bağlandı!");
        state.mqttConnected = true;
        publishStatus();
      } else {
        Serial.println(" Başarısız, rc=" + String(mqttClient.state()));
        state.mqttConnected = false;
      }
    }
  } else {
    mqttClient.loop();
    state.mqttConnected = true;

    // Canlı sensör verisi gönderimi (interval kaldırıldı)
    publishSensors();
    delay(500); // Okunabilirlik için 500ms bekleme (canlı hissi devam eder)
  }
}

void publishStatus() {
  if (!mqttClient.connected())
    return;

  JsonDocument doc;
  doc["deviceId"] = state.deviceId;
  doc["status"] = "online";
  doc["ip"] = WiFi.localIP().toString();
  doc["version"] = FIRMWARE_VERSION;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = getUptimeString();

  // Konfigürasyon detayları
  JsonObject config = doc["config"].to<JsonObject>();
  config["ssid"] = state.savedSSID;
  config["mqtt_host"] = state.mqttHost;
  config["mqtt_port"] = state.mqttPort;
  config["read_interval"] = DEFAULT_READ_INTERVAL; // Veya state'den
  config["ntp_server"] = "tr.pool.ntp.org";
  config["sleep_mode"] = "Deaktif";

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  // Gerçek sensör değerleri (DHT/I2C sensörler eklendiğinde burası
  // güncellenebilir)
  sensors["nem"] = random(45, 55);      // Simüle
  sensors["sicaklik"] = random(23, 25); // Simüle

  String payload;
  serializeJson(doc, payload);

  String topic = "iot_esp_101/devices/" + state.deviceId + "/status";
  mqttClient.publish(topic.c_str(), payload.c_str(), true);
  Serial.println("[MQTT] Status gönderildi: " + topic);
}

void publishSensors() {

  JsonDocument doc;
  doc["serial_type"] = "status";
  doc["deviceId"] = state.deviceId;
  doc["timestamp"] = millis();

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  // GERÇEK SENSÖR OKUMALARI
  sensors["nem"] = random(45, 55);      // Simüle
  sensors["sicaklik"] = random(23, 25); // Simüle

  // Analog Sensörler (Pinlerden gerçek okuma yapılır)
  sensors["uv"] = analogRead(PIN_UV_ANALOG);
  sensors["yagmur"] = analogRead(PIN_RAIN_ANALOG);
  sensors["toprak_nem"] = analogRead(PIN_SOIL_MOISTURE);

  // Serial üzerinden her durumda gönder (WebSerial için)
  Serial.print("JSON_STATUS:");
  serializeJson(doc, Serial);
  Serial.println();

  // MQTT üzerinden gönder (Eğer bağlıysa)
  if (mqttClient.connected()) {
    String payload;
    serializeJson(doc, payload);
    String topic = "iot_esp_101/devices/" + state.deviceId + "/sensors";
    mqttClient.publish(topic.c_str(), payload.c_str());
  }

  // Detaylı Serial Çıktısı
  Serial.println("[MQTT/SERIAL] Sensör Verisi Okundu:");
  Serial.print("  > UV: ");
  Serial.println(sensors["uv"].as<int>());
  Serial.print("  > Yagmur: ");
  Serial.println(sensors["yagmur"].as<int>());
  Serial.print("  > Toprak Nem: ");
  Serial.println(sensors["toprak_nem"].as<int>());
  Serial.println("-------------------------");
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.print("[MQTT] Mesaj alındı [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

String generateDashboardHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="tr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>iot_esp_101 Dashboard</title>
    <style>
        :root {
            --bg-primary: #0f0f23;
            --bg-secondary: #1a1a3e;
            --bg-card: linear-gradient(135deg, #1e1e4a 0%, #2a2a5a 100%);
            --accent-primary: #00d4aa;
            --accent-secondary: #7c3aed;
            --accent-warning: #fbbf24;
            --accent-danger: #ef4444;
            --text-primary: #ffffff;
            --text-secondary: #a1a1c7;
            --border-color: rgba(124, 58, 237, 0.3);
        }
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', system-ui, sans-serif;
            background: var(--bg-primary);
            color: var(--text-primary);
            min-height: 100vh;
        }
        body::before {
            content: '';
            position: fixed;
            top: 0; left: 0; width: 100%; height: 100%;
            background: 
                radial-gradient(ellipse at 20% 20%, rgba(124, 58, 237, 0.1) 0%, transparent 50%),
                radial-gradient(ellipse at 80% 80%, rgba(0, 212, 170, 0.1) 0%, transparent 50%);
            pointer-events: none;
            z-index: -1;
        }
        .container { max-width: 1200px; margin: 0 auto; padding: 20px; }
        header { text-align: center; padding: 40px 20px; }
        .logo { font-size: 3rem; margin-bottom: 10px; animation: float 3s ease-in-out infinite; }
        @keyframes float {
            0%, 100% { transform: translateY(0); }
            50% { transform: translateY(-10px); }
        }
        h1 {
            font-size: 2.5rem;
            background: linear-gradient(135deg, var(--accent-primary), var(--accent-secondary));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .subtitle { color: var(--text-secondary); font-size: 1.1rem; }
        .status-badge {
            display: inline-flex;
            align-items: center;
            gap: 8px;
            padding: 8px 16px;
            border-radius: 30px;
            font-size: 0.9rem;
            margin-top: 15px;
            background: rgba(0, 212, 170, 0.2);
            color: var(--accent-primary);
            border: 1px solid rgba(0, 212, 170, 0.3);
        }
        .pulse {
            width: 10px; height: 10px;
            border-radius: 50%;
            background: var(--accent-primary);
            animation: pulse 2s ease-in-out infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; transform: scale(1); }
            50% { opacity: 0.5; transform: scale(1.2); }
        }
        .datetime-display {
            background: var(--bg-card);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            padding: 30px;
            text-align: center;
            margin-bottom: 30px;
        }
        .time {
            font-size: 4rem;
            font-weight: 700;
            font-family: 'Courier New', monospace;
            color: var(--accent-primary);
            text-shadow: 0 0 30px rgba(0, 212, 170, 0.5);
        }
        .date { font-size: 1.3rem; color: var(--text-secondary); margin-top: 10px; }
        .cards-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 20px; }
        .card {
            background: var(--bg-card);
            border: 1px solid var(--border-color);
            border-radius: 16px;
            padding: 24px;
            transition: transform 0.3s;
        }
        .card:hover { transform: translateY(-5px); }
        .card-header { display: flex; align-items: center; gap: 12px; margin-bottom: 20px; }
        .card-icon {
            width: 48px; height: 48px;
            border-radius: 12px;
            display: flex; align-items: center; justify-content: center;
            font-size: 1.5rem;
        }
        .card-icon.network { background: linear-gradient(135deg, #3b82f6, #1d4ed8); }
        .card-icon.device { background: linear-gradient(135deg, #8b5cf6, #6d28d9); }
        .card-icon.memory { background: linear-gradient(135deg, #10b981, #059669); }
        .card-icon.settings { background: linear-gradient(135deg, #f59e0b, #d97706); }
        .card-title { font-size: 1.1rem; font-weight: 600; }
        .info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid rgba(255,255,255,0.05); }
        .info-row:last-child { border-bottom: none; }
        .info-label { color: var(--text-secondary); font-size: 0.9rem; }
        .info-value { font-weight: 500; font-family: 'Courier New', monospace; color: var(--accent-primary); }
        .btn {
            width: 100%;
            padding: 12px;
            border: none;
            border-radius: 10px;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            margin-top: 15px;
            transition: all 0.3s;
        }
        .btn-settings {
            background: transparent;
            border: 1px solid var(--accent-warning);
            color: var(--accent-warning);
        }
        .btn-settings:hover { background: rgba(251, 191, 36, 0.1); }
        footer { text-align: center; padding: 30px; color: var(--text-secondary); }
        @media (max-width: 768px) {
            h1 { font-size: 1.8rem; }
            .time { font-size: 2.5rem; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <div class="logo">🌱</div>
            <h1>iot_esp_101 Sistemi</h1>
            <p class="subtitle">Deneyap Kart 1A - Dashboard</p>
            <div class="status-badge">
                <span class="pulse"></span>
                <span>Çevrimiçi</span>
            </div>
        </header>
        
        <div class="datetime-display">
            <div class="time" id="currentTime">--:--:--</div>
            <div class="date" id="currentDate">Yükleniyor...</div>
        </div>
        
        <div class="cards-grid">
            <div class="card">
                <div class="card-header">
                    <div class="card-icon network">📡</div>
                    <span class="card-title">Ağ Bilgileri</span>
                </div>
                <div class="info-row">
                    <span class="info-label">IP Adresi</span>
                    <span class="info-value" id="ipAddress">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">MAC Adresi</span>
                    <span class="info-value" id="macAddress">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">WiFi SSID</span>
                    <span class="info-value" id="ssid">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Sinyal</span>
                    <span class="info-value" id="rssi">---</span>
                </div>
            </div>
            
            <div class="card">
                <div class="card-header">
                    <div class="card-icon device">🔧</div>
                    <span class="card-title">Cihaz</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Cihaz ID</span>
                    <span class="info-value" id="deviceId">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Firmware</span>
                    <span class="info-value" id="firmware">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Uptime</span>
                    <span class="info-value" id="uptime">---</span>
                </div>
            </div>
            
            <div class="card">
                <div class="card-header">
                    <div class="card-icon memory">💾</div>
                    <span class="card-title">Sistem</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Boş Bellek</span>
                    <span class="info-value" id="freeHeap">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">CPU</span>
                    <span class="info-value" id="cpuFreq">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Flash</span>
                    <span class="info-value" id="flashSize">---</span>
                </div>
            </div>
            
            <div class="card">
                <div class="card-header">
                    <div class="card-icon settings">⚙️</div>
                    <span class="card-title">MQTT Sunucu</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Host</span>
                    <span class="info-value" id="mqttHost">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Port</span>
                    <span class="info-value" id="mqttPort">---</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Durum</span>
                    <span class="info-value" id="mqttStatus">---</span>
                </div>
                <button class="btn btn-settings" onclick="if(confirm('Kurulum moduna geçmek istiyor musunuz?')) fetch('/reset', {method:'POST'})">
                    🔄 Ayarları Yeniden Yap
                </button>
            </div>
        </div>
        
        <footer>
            <p>Son Güncelleme: <span id="lastUpdate">---</span></p>
        </footer>
    </div>
    
    <script>
        async function fetchData() {
            try {
                const res = await fetch('/api/system');
                const d = await res.json();
                
                document.getElementById('currentTime').textContent = d.time || '--:--:--';
                document.getElementById('currentDate').textContent = d.date || '---';
                document.getElementById('ipAddress').textContent = d.ip || '---';
                document.getElementById('macAddress').textContent = d.mac || '---';
                document.getElementById('ssid').textContent = d.ssid || '---';
                document.getElementById('rssi').textContent = d.rssi + ' dBm (' + d.signalQuality + '%)';
                document.getElementById('deviceId').textContent = d.deviceId || '---';
                document.getElementById('firmware').textContent = 'v' + d.firmware;
                document.getElementById('uptime').textContent = d.uptime || '---';
                document.getElementById('freeHeap').textContent = (d.freeHeap/1024).toFixed(1) + ' KB';
                document.getElementById('cpuFreq').textContent = d.cpuFreq + ' MHz';
                document.getElementById('flashSize').textContent = d.flashSize + ' MB';
                document.getElementById('mqttHost').textContent = d.mqttHost || 'Ayarlanmadı';
                document.getElementById('mqttPort').textContent = d.mqttPort || '---';
                document.getElementById('mqttStatus').textContent = d.mqttConnected ? '✅ Bağlı' : '⚠️ Bağlı değil';
                document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString('tr-TR');
            } catch(e) { console.error(e); }
        }
        fetchData();
        setInterval(fetchData, 2000);
    </script>
</body>
</html>
)rawliteral";
}

// ============================================
// HELPER FUNCTIONS
// ============================================
String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "--:--:--";
  char buf[12];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

String getFormattedDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "---";
  const char *days[] = {"Pazar",    "Pazartesi", "Salı",     "Çarşamba",
                        "Perşembe", "Cuma",      "Cumartesi"};
  const char *months[] = {"Ocak",  "Şubat",   "Mart",   "Nisan",
                          "Mayıs", "Haziran", "Temmuz", "Ağustos",
                          "Eylül", "Ekim",    "Kasım",  "Aralık"};
  return String(timeinfo.tm_mday) + " " + months[timeinfo.tm_mon] + " " +
         String(timeinfo.tm_year + 1900) + ", " + days[timeinfo.tm_wday];
}

String getUptimeString() {
  unsigned long s = state.uptime / 1000;
  unsigned long m = s / 60;
  s %= 60;
  unsigned long h = m / 60;
  m %= 60;
  unsigned long d = h / 24;
  h %= 24;
  String r = "";
  if (d > 0)
    r += String(d) + "g ";
  if (h > 0 || d > 0)
    r += String(h) + "s ";
  if (m > 0 || h > 0 || d > 0)
    r += String(m) + "d ";
  r += String(s) + "sn";
  return r;
}

int getSignalQuality(int rssi) {
  if (rssi >= -50)
    return 100;
  if (rssi >= -60)
    return 80;
  if (rssi >= -70)
    return 60;
  if (rssi >= -80)
    return 40;
  if (rssi >= -90)
    return 20;
  return 0;
}
