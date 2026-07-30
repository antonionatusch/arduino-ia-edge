#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

#if __has_include("WiFiCredentials.h")
#include "WiFiCredentials.h"
#else
#define SSID ""
#define PASSWORD ""
#define API_TOKEN "change-me"
#endif

constexpr int RELAY_PIN = 26;
constexpr bool RELAY_ACTIVE_LOW = false;
constexpr int RELAY_ENABLED_LEVEL = RELAY_ACTIVE_LOW ? LOW : HIGH;
constexpr int RELAY_DISABLED_LEVEL = RELAY_ACTIVE_LOW ? HIGH : LOW;

constexpr char WIFI_SSID[] = SSID;
constexpr char WIFI_PASSWORD[] = PASSWORD;
constexpr char HTTP_API_TOKEN[] = API_TOKEN;
constexpr char HOSTNAME[] = "esp32-master";

constexpr long GMT_OFFSET_SECONDS = -4L * 3600L;
constexpr int DAYLIGHT_OFFSET_SECONDS = 0;
constexpr char NTP_SERVER_1[] = "pool.ntp.org";
constexpr char NTP_SERVER_2[] = "time.nist.gov";

// Cada ronda enciende a hh:59:30 y apaga a hh:05:20. Las rondas validas
// comienzan cada hora entre las 08:00 y las 22:00, hora Bolivia (UTC-4).
constexpr int FIRST_ROUND_HOUR = 8;
constexpr int LAST_ROUND_HOUR = 22;
constexpr int PREWAKE_SECOND_OF_HOUR = 59 * 60 + 30;
constexpr int SHUTDOWN_SECOND_OF_HOUR = 5 * 60 + 20;
constexpr unsigned long WIFI_RETRY_MS = 15000;
constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 UTC

enum class OperatingMode {
  AUTOMATIC,
  MANUAL_ON,
  MANUAL_OFF,
};

WebServer server(80);
OperatingMode operatingMode = OperatingMode::AUTOMATIC;
bool relayEnabled = false;
bool timeConfigured = false;
unsigned long lastWifiAttempt = 0;
esp_reset_reason_t lastResetReason = ESP_RST_UNKNOWN;

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
      return "task_watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep_sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    case ESP_RST_UNKNOWN:
    default:
      return "unknown";
  }
}

const char *modeName(OperatingMode mode) {
  switch (mode) {
    case OperatingMode::AUTOMATIC:
      return "automatic";
    case OperatingMode::MANUAL_ON:
      return "manual_on";
    case OperatingMode::MANUAL_OFF:
      return "manual_off";
  }
  return "unknown";
}

void setRelay(bool enabled) {
  if (relayEnabled == enabled) {
    return;
  }

  relayEnabled = enabled;
  digitalWrite(RELAY_PIN, enabled ? RELAY_ENABLED_LEVEL : RELAY_DISABLED_LEVEL);

  Serial.printf("ESP32-CAM y ventilador: %s\n",
                enabled ? "ENCENDIDOS" : "APAGADOS");
}

bool getCurrentTime(struct tm &timeInfo) {
  time_t now;
  time(&now);
  if (now < MIN_VALID_EPOCH) {
    return false;
  }
  return getLocalTime(&timeInfo, 0);
}

bool shouldRelayBeEnabled(const struct tm &timeInfo) {
  const int secondOfHour = timeInfo.tm_min * 60 + timeInfo.tm_sec;

  const bool activeRound =
      timeInfo.tm_hour >= FIRST_ROUND_HOUR &&
      timeInfo.tm_hour <= LAST_ROUND_HOUR &&
      secondOfHour < SHUTDOWN_SECOND_OF_HOUR;

  const bool prewakeForNextRound =
      timeInfo.tm_hour >= FIRST_ROUND_HOUR - 1 &&
      timeInfo.tm_hour < LAST_ROUND_HOUR &&
      secondOfHour >= PREWAKE_SECOND_OF_HOUR;

  return activeRound || prewakeForNextRound;
}

void applyOperatingMode() {
  if (operatingMode == OperatingMode::MANUAL_ON) {
    setRelay(true);
    return;
  }

  if (operatingMode == OperatingMode::MANUAL_OFF) {
    setRelay(false);
    return;
  }

  struct tm timeInfo {};
  if (!getCurrentTime(timeInfo)) {
    setRelay(false);
    return;
  }

  setRelay(shouldRelayBeEnabled(timeInfo));
}

bool isAuthorized() {
  if (server.header("X-API-Key") == HTTP_API_TOKEN) {
    return true;
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(401, "application/json", "{\"detail\":\"invalid_api_key\"}");
  return false;
}

void sendJson(int statusCode, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json", json);
}

void handleStatus() {
  struct tm timeInfo {};
  const bool timeSynchronized = getCurrentTime(timeInfo);

  char localTime[24] = "unavailable";
  if (timeSynchronized) {
    strftime(localTime, sizeof(localTime), "%Y-%m-%dT%H:%M:%S", &timeInfo);
  }

  String json;
  json.reserve(384);
  json += "{";
  json += "\"device\":\"esp32-master\",";
  json += "\"mode\":\"" + String(modeName(operatingMode)) + "\",";
  json += "\"relay_enabled\":" + String(relayEnabled ? "true" : "false") + ",";
  json += "\"time_synchronized\":" + String(timeSynchronized ? "true" : "false") + ",";
  json += "\"local_time\":\"" + String(localTime) + "\",";
  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifi_rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime_ms\":" + String(millis()) + ",";
  json += "\"reset_reason\":\"" + String(resetReasonName(lastResetReason)) + "\",";
  json += "\"schedule\":{\"round_hours\":\"08:00-22:00\",\"on\":\"hh:59:30\",\"off\":\"hh:05:20\"}";
  json += "}";
  sendJson(200, json);
}

void handleMode() {
  if (!isAuthorized()) {
    return;
  }

  String requestedMode = server.arg("value");
  requestedMode.toLowerCase();

  if (requestedMode == "automatic") {
    operatingMode = OperatingMode::AUTOMATIC;
  } else if (requestedMode == "manual_on") {
    operatingMode = OperatingMode::MANUAL_ON;
  } else if (requestedMode == "manual_off") {
    operatingMode = OperatingMode::MANUAL_OFF;
  } else {
    sendJson(400, "{\"detail\":\"invalid_mode\"}");
    return;
  }

  applyOperatingMode();
  Serial.printf("Modo cambiado a: %s\n", modeName(operatingMode));
  handleStatus();
}

void handleDebugRelay() {
  if (!isAuthorized()) {
    return;
  }

  String requestedState = server.arg("enabled");
  requestedState.toLowerCase();
  if (requestedState == "true" || requestedState == "1") {
    operatingMode = OperatingMode::MANUAL_ON;
  } else if (requestedState == "false" || requestedState == "0") {
    operatingMode = OperatingMode::MANUAL_OFF;
  } else {
    sendJson(400, "{\"detail\":\"invalid_relay_state\"}");
    return;
  }

  applyOperatingMode();
  handleStatus();
}

void startHttpServer() {
  const char *headers[] = {"X-API-Key"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8",
                "ESP32 maestro listo. Use GET /api/v1/status");
  });
  server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.on("/api/v1/mode", HTTP_POST, handleMode);
  server.on("/api/v1/debug/relay", HTTP_POST, handleDebugRelay);
  server.onNotFound([]() {
    sendJson(404, "{\"detail\":\"endpoint_not_found\"}");
  });
  server.begin();
  Serial.println("Servidor HTTP iniciado en el puerto 80.");
}

void beginWifiConnection() {
  lastWifiAttempt = millis();
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Conectando a Wi-Fi '%s'...\n", WIFI_SSID);
}

void maintainWifiAndTime() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeConfigured) {
      configTime(GMT_OFFSET_SECONDS,
                 DAYLIGHT_OFFSET_SECONDS,
                 NTP_SERVER_1,
                 NTP_SERVER_2);
      timeConfigured = true;
      Serial.printf("Wi-Fi conectado. IP: %s\n",
                    WiFi.localIP().toString().c_str());
      Serial.println("Sincronizacion NTP solicitada.");
    }
    return;
  }

  timeConfigured = false;
  if (millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    beginWifiConnection();
  }
}

void setup() {
  Serial.begin(115200);
  lastResetReason = esp_reset_reason();

  // Set the output latch before enabling the pin to avoid a relay pulse.
  digitalWrite(RELAY_PIN, RELAY_DISABLED_LEVEL);
  pinMode(RELAY_PIN, OUTPUT);
  relayEnabled = false;

  Serial.printf("Motivo del ultimo reinicio: %s\n",
                resetReasonName(lastResetReason));

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  beginWifiConnection();
  startHttpServer();
}

void loop() {
  server.handleClient();
  maintainWifiAndTime();
  applyOperatingMode();
  delay(5);
}
