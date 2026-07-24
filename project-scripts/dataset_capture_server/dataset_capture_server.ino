#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "WiFiCredentials.h"

// Replace these values before uploading the sketch.
const char *WIFI_SSID = SSID;
const char *WIFI_PASSWORD = PASSWORD;

// AI Thinker ESP32-CAM pinout. Change this block if your GC2145 board uses
// a different camera connector or board definition.
constexpr int PWDN_GPIO_NUM = 32;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM = 0;
constexpr int SIOD_GPIO_NUM = 26;
constexpr int SIOC_GPIO_NUM = 27;
constexpr int Y9_GPIO_NUM = 35;
constexpr int Y8_GPIO_NUM = 34;
constexpr int Y7_GPIO_NUM = 39;
constexpr int Y6_GPIO_NUM = 36;
constexpr int Y5_GPIO_NUM = 21;
constexpr int Y4_GPIO_NUM = 19;
constexpr int Y3_GPIO_NUM = 18;
constexpr int Y2_GPIO_NUM = 5;
constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM = 23;
constexpr int PCLK_GPIO_NUM = 22;

constexpr uint16_t EXPECTED_SENSOR_PID = 0x2145;
constexpr size_t EXPECTED_RGB565_BYTES = 160U * 120U * 2U;
constexpr unsigned long WIFI_RETRY_MS = 15000;

WebServer server(80);
uint16_t detectedSensorPid = 0;
unsigned long lastWifiRetry = 0;

void printMemoryDiagnostics(const char *context) {
  Serial.printf("[%s] Heap libre: %u bytes, heap minimo: %u bytes, PSRAM libre: %u bytes\n",
                context,
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                ESP.getFreePsram());
}

void sendTextError(int statusCode, const String &message) {
  Serial.printf("HTTP %d: %s\n", statusCode, message.c_str());
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "text/plain; charset=utf-8", message);
}

void handleStatus() {
  String json = "{";
  json += "\"camera_ready\":true,";
  json += "\"sensor_pid\":\"0x" + String(detectedSensorPid, HEX) + "\",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
  json += "\"format\":\"RGB565\",";
  json += "\"resolution\":\"160x120\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCapture() {
  printMemoryDiagnostics("antes de captura");

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    sendTextError(503, "No se pudo obtener un framebuffer de la camara.");
    return;
  }

  Serial.printf("Framebuffer: %ux%u, formato=%d, longitud=%u bytes\n",
                frame->width,
                frame->height,
                frame->format,
                frame->len);

  if (frame->format != PIXFORMAT_RGB565 || frame->width != 160 ||
      frame->height != 120 || frame->len != EXPECTED_RGB565_BYTES) {
    String details = "Framebuffer inesperado: " + String(frame->width) + "x" +
                     String(frame->height) + ", formato=" + String(frame->format) +
                     ", bytes=" + String(frame->len);
    esp_camera_fb_return(frame);
    sendTextError(500, details);
    return;
  }

  uint8_t *bmpBuffer = nullptr;
  size_t bmpLength = 0;
  const bool converted = frame2bmp(frame, &bmpBuffer, &bmpLength);

  // The camera owns the framebuffer; return it immediately after conversion.
  esp_camera_fb_return(frame);
  frame = nullptr;

  if (!converted || bmpBuffer == nullptr || bmpLength == 0) {
    if (bmpBuffer != nullptr) {
      free(bmpBuffer);
    }
    sendTextError(500, "frame2bmp() no pudo convertir la captura RGB565.");
    return;
  }

  Serial.printf("BMP convertido: %u bytes\n", bmpLength);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("X-Sensor-PID", "0x" + String(detectedSensorPid, HEX));
  server.sendHeader("X-Framebuffer-Bytes", String(EXPECTED_RGB565_BYTES));
  server.setContentLength(bmpLength);
  server.send(200, "image/bmp", "");
  server.sendContent(reinterpret_cast<const char *>(bmpBuffer), bmpLength);

  free(bmpBuffer);
  bmpBuffer = nullptr;
  printMemoryDiagnostics("despues de respuesta");
}

bool initializeCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("esp_camera_init() fallo con codigo 0x%x\n", error);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    Serial.println("No se pudo consultar el sensor despues de inicializarlo.");
    esp_camera_deinit();
    return false;
  }

  detectedSensorPid = sensor->id.PID;
  Serial.printf("Sensor detectado: PID=0x%04x (esperado GC2145=0x%04x)\n",
                detectedSensorPid,
                EXPECTED_SENSOR_PID);
  if (detectedSensorPid != EXPECTED_SENSOR_PID) {
    Serial.println("ADVERTENCIA: el PID no coincide con el GC2145 comprobado.");
  }

  return true;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Conectando a Wi-Fi '%s'", WIFI_SSID);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_RETRY_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi conectado. IP: %s, RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.println("No se pudo conectar. Se reintentara desde loop().");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nServidor de captura de dataset ESP32-CAM / GC2145");
  Serial.printf("PSRAM detectada: %s, total: %u bytes\n",
                psramFound() ? "si" : "no",
                ESP.getPsramSize());
  printMemoryDiagnostics("inicio");

  if (!initializeCamera()) {
    Serial.println("La camara no pudo inicializarse. Reinicie despues de revisar cableado y pines.");
    return;
  }

  connectWifi();
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8",
                "ESP32-CAM GC2145 listo. Use GET /status o GET /capture");
  });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);
  server.onNotFound([]() { sendTextError(404, "Endpoint no encontrado."); });
  server.begin();
  Serial.println("Servidor HTTP iniciado en el puerto 80.");
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    Serial.println("Wi-Fi desconectado; intentando reconectar...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  delay(2);
}
