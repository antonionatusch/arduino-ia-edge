#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
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
constexpr uint8_t DEFAULT_STREAM_FPS = 4;
constexpr uint8_t MAX_STREAM_FPS = 8;
constexpr uint8_t NO_STREAM_CLIENT = 0xFF;

WebServer server(80);
WebSocketsServer webSocket(81);
uint16_t detectedSensorPid = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastStreamFrame = 0;
uint32_t streamedFrames = 0;
uint8_t streamClient = NO_STREAM_CLIENT;
uint8_t streamFps = DEFAULT_STREAM_FPS;
bool streamActive = false;

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

bool captureBmp(uint8_t **bmpBuffer, size_t *bmpLength, String &errorMessage) {
  *bmpBuffer = nullptr;
  *bmpLength = 0;

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    errorMessage = "No se pudo obtener un framebuffer de la camara.";
    return false;
  }

  if (frame->format != PIXFORMAT_RGB565 || frame->width != 160 ||
      frame->height != 120 || frame->len != EXPECTED_RGB565_BYTES) {
    errorMessage = "Framebuffer inesperado: " + String(frame->width) + "x" +
                   String(frame->height) + ", formato=" + String(frame->format) +
                   ", bytes=" + String(frame->len);
    esp_camera_fb_return(frame);
    return false;
  }

  const bool converted = frame2bmp(frame, bmpBuffer, bmpLength);
  esp_camera_fb_return(frame);

  if (!converted || *bmpBuffer == nullptr || *bmpLength == 0) {
    if (*bmpBuffer != nullptr) {
      free(*bmpBuffer);
      *bmpBuffer = nullptr;
    }
    *bmpLength = 0;
    errorMessage = "frame2bmp() no pudo convertir la captura RGB565.";
    return false;
  }

  return true;
}

void handleStatus() {
  String json = "{";
  json += "\"camera_ready\":true,";
  json += "\"sensor_pid\":\"0x" + String(detectedSensorPid, HEX) + "\",";
  json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
  json += "\"format\":\"RGB565\",";
  json += "\"resolution\":\"160x120\",";
  json += "\"websocket_port\":81,";
  json += "\"stream_fps\":" + String(streamFps) + ",";
  json += "\"stream_active\":" + String(streamActive ? "true" : "false");
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCapture() {
  printMemoryDiagnostics("antes de captura");

  uint8_t *bmpBuffer = nullptr;
  size_t bmpLength = 0;
  String errorMessage;
  if (!captureBmp(&bmpBuffer, &bmpLength, errorMessage)) {
    sendTextError(503, errorMessage);
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

void handleWebSocketEvent(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    IPAddress remoteIp = webSocket.remoteIP(client);
    Serial.printf("WebSocket cliente %u conectado desde %s\n",
                  client,
                  remoteIp.toString().c_str());
    webSocket.sendTXT(client, "READY");
    return;
  }

  if (type == WStype_DISCONNECTED) {
    Serial.printf("WebSocket cliente %u desconectado.\n", client);
    if (client == streamClient) {
      streamActive = false;
      streamClient = NO_STREAM_CLIENT;
    }
    return;
  }

  if (type != WStype_TEXT) {
    return;
  }

  String command;
  command.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    command += static_cast<char>(payload[index]);
  }
  command.trim();
  command.toUpperCase();
  Serial.printf("WebSocket cliente %u: %s\n", client, command.c_str());

  if (command == "START") {
    if (streamClient != NO_STREAM_CLIENT && streamClient != client) {
      webSocket.sendTXT(client, "ERROR:STREAM_BUSY");
      return;
    }
    streamClient = client;
    streamActive = true;
    lastStreamFrame = 0;
    webSocket.sendTXT(client, "STREAM_STARTED");
  } else if (command == "PAUSE") {
    if (client == streamClient) {
      streamActive = false;
      webSocket.sendTXT(client, "STREAM_PAUSED");
    }
  } else if (command == "STOP") {
    if (client == streamClient) {
      streamActive = false;
      streamClient = NO_STREAM_CLIENT;
      webSocket.sendTXT(client, "STREAM_STOPPED");
    }
  } else if (command.startsWith("FPS:")) {
    const int requestedFps = command.substring(4).toInt();
    if (requestedFps < 1 || requestedFps > MAX_STREAM_FPS) {
      webSocket.sendTXT(client, "ERROR:FPS_RANGE_1_8");
      return;
    }
    streamFps = static_cast<uint8_t>(requestedFps);
    String response = "FPS:" + String(streamFps);
    webSocket.sendTXT(client, response);
  } else {
    webSocket.sendTXT(client, "ERROR:UNKNOWN_COMMAND");
  }
}

void sendStreamFrame() {
  uint8_t *bmpBuffer = nullptr;
  size_t bmpLength = 0;
  String errorMessage;
  if (!captureBmp(&bmpBuffer, &bmpLength, errorMessage)) {
    Serial.println("Error de stream: " + errorMessage);
    webSocket.sendTXT(streamClient, "ERROR:CAPTURE_FAILED");
    return;
  }

  const bool sent = webSocket.sendBIN(streamClient, bmpBuffer, bmpLength);
  free(bmpBuffer);
  bmpBuffer = nullptr;

  if (!sent) {
    Serial.println("No se pudo enviar el frame WebSocket; stream pausado.");
    streamActive = false;
    return;
  }

  ++streamedFrames;
  if (streamedFrames % 40 == 0) {
    Serial.printf("Stream: %u frames, ultimo BMP=%u bytes, FPS objetivo=%u\n",
                  streamedFrames,
                  bmpLength,
                  streamFps);
    printMemoryDiagnostics("stream");
  }
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

  webSocket.begin();
  webSocket.onEvent(handleWebSocketEvent);
  Serial.printf("Servidor WebSocket iniciado en el puerto 81 a %u FPS.\n", streamFps);
}

void loop() {
  server.handleClient();
  webSocket.loop();

  const unsigned long streamInterval = 1000UL / streamFps;
  if (streamActive && WiFi.status() == WL_CONNECTED &&
      millis() - lastStreamFrame >= streamInterval) {
    lastStreamFrame = millis();
    sendStreamFrame();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    Serial.println("Wi-Fi desconectado; intentando reconectar...");
    streamActive = false;
    streamClient = NO_STREAM_CLIENT;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  delay(2);
}
