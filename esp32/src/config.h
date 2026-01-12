#ifndef CONFIG_H
#define CONFIG_H

// ==============================================
// FIRMWARE BİLGİLERİ
// ==============================================
#define DEVICE_NAME "iot_esp_101 iot_esp_101 Sistemi"
#define DEVICE_ID "deneyap-001"
#define FIRMWARE_VERSION "1.0.0"

// ==============================================
// Access Point (Kurulum Modu) Ayarları
// ==============================================
#define AP_SSID "espressif - esp32 - deneyapkart" // Kurulum WiFi adı
#define AP_PASSWORD "deneyapkart" // Kurulum WiFi şifresi (min 8 karakter)
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

// ==============================================
// Web Sunucu Ayarları
// ==============================================
#define WEB_SERVER_PORT 80
#define DNS_PORT 53

// ==============================================
// NTP (Network Time Protocol) Ayarları
// ==============================================
#define NTP_SERVER "pool.ntp.org"
#define NTP_GMT_OFFSET 10800 // Türkiye için GMT+3 (saniye: 3*3600)
#define NTP_DAYLIGHT_OFFSET 0

// ==============================================
// MQTT Ayarları (Backend'den alınacak)
// ==============================================
#define MQTT_PORT 1883
#define MQTT_KEEPALIVE 60

// MQTT Topic'leri
#define MQTT_TOPIC_BASE "iot_esp_101/devices/"
#define MQTT_TOPIC_STATUS "/status"
#define MQTT_TOPIC_SENSORS_SOIL "/sensors/soil"
#define MQTT_TOPIC_SENSORS_ENV "/sensors/environment"
#define MQTT_TOPIC_SENSORS_UV "/sensors/uv"
#define MQTT_TOPIC_SENSORS_RAIN "/sensors/rain"

// ==============================================
// Sensör Okuma Aralıkları (ms)
// ==============================================
#define SENSOR_READ_INTERVAL 5000   // 5 saniye
#define MQTT_PUBLISH_INTERVAL 10000 // 10 saniye

// ==============================================
// Pin Tanımlamaları (Deneyap Kart 1A)
// ==============================================
// I2C (Tüm I2C sensörler için)
#define I2C_SDA 21
#define I2C_SCL 22

// Analog Pinler
#define PIN_SOIL_MOISTURE 36 // Toprak Nem Sensörü (ADC)
#define PIN_RAIN_ANALOG 39   // Yağmur Sensörü Analog
#define PIN_UV_ANALOG 34     // UV Sensörü Analog (Örn: GPIO34)

// Digital Pinler
#define PIN_RAIN_DIGITAL 4 // Yağmur Sensörü Digital (varsa)

// Dahili LED (RGB - Adreslenebilir)
#define PIN_RGB_LED 12 // Dahili RGB LED

// Dahili Buton
#define PIN_BUTTON 8 // Genel amaçlı buton

// ==============================================
// NVS (Non-Volatile Storage) Keys
// ==============================================
#define NVS_NAMESPACE "iot_config"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_MQTT_HOST "mqtt_host"
#define NVS_KEY_MQTT_PORT "mqtt_port"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_CONFIGURED "configured"

// Extended Config Keys
#define NVS_KEY_READ_INTERVAL "read_int"
#define NVS_KEY_SENSORS_ENABLED "sensors"
#define NVS_KEY_SLEEP_ENABLED "sleep_en"
#define NVS_KEY_SLEEP_MINUTES "sleep_min"
#define NVS_KEY_ALARM_NEM_MIN "alarm_nmin"
#define NVS_KEY_ALARM_NEM_MAX "alarm_nmax"
#define NVS_KEY_ALARM_TEMP_MAX "alarm_tmax"
#define NVS_KEY_NTP_SERVER "ntp_srv"
#define NVS_KEY_TIMEZONE "timezone"
#define NVS_KEY_DEBUG_LEVEL "debug_lvl"
#define NVS_KEY_LED_ENABLED "led_en"

// ==============================================
// Default Config Values
// ==============================================
#define DEFAULT_READ_INTERVAL 30     // 30 saniye
#define DEFAULT_SENSORS_ENABLED 0x0F // Tüm sensörler aktif (4 bit)
#define DEFAULT_SLEEP_ENABLED false
#define DEFAULT_SLEEP_MINUTES 5
#define DEFAULT_ALARM_NEM_MIN 20
#define DEFAULT_ALARM_NEM_MAX 80
#define DEFAULT_ALARM_TEMP_MAX 40
#define DEFAULT_DEBUG_LEVEL 1 // 0=Off, 1=Error, 2=Info, 3=Verbose
#define DEFAULT_LED_ENABLED true

// Sensor bit flags
#define SENSOR_NEM 0x01
#define SENSOR_SICAKLIK 0x02
#define SENSOR_UV 0x04
#define SENSOR_YAGMUR 0x08

// ==============================================
// Timeout Ayarları
// ==============================================
#define WIFI_CONNECT_TIMEOUT_MS 15000   // WiFi bağlantı timeout
#define WIFI_SCAN_TIMEOUT_MS 10000      // WiFi tarama timeout
#define CONFIG_PORTAL_TIMEOUT_MS 300000 // 5 dakika konfigürasyon portal timeout

#endif // CONFIG_H
