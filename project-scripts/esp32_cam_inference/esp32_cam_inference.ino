#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "img_converters.h"

#include <pet-feeder-monitor_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

#if __has_include("WiFiCredentials.h")
#include "WiFiCredentials.h"
#else
#define SSID ""
#define PASSWORD ""
#endif

// AI Thinker pinout used by the GC2145 camera board.
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

constexpr char WIFI_SSID[] = SSID;
constexpr char WIFI_PASSWORD[] = PASSWORD;
constexpr char HOSTNAME[] = "esp32-cam-inference";
constexpr char FIRMWARE_VERSION[] = "1.0.0";
constexpr char MODEL_VERSION[] = "edge-impulse-deploy-3";

constexpr uint16_t EXPECTED_SENSOR_PID = 0x2145;
constexpr uint16_t CAMERA_WIDTH = 320;
constexpr uint16_t CAMERA_HEIGHT = 240;
constexpr size_t RGB565_BYTES = CAMERA_WIDTH * CAMERA_HEIGHT * 2U;
constexpr size_t RGB888_BYTES = CAMERA_WIDTH * CAMERA_HEIGHT * 3U;
constexpr float MIN_CLASSIFICATION_CONFIDENCE = 0.50F;
constexpr unsigned long WIFI_RETRY_MS = 15000;
constexpr uint8_t DEFAULT_STREAM_FPS = 2;
constexpr uint8_t MAX_STREAM_FPS = 4;
constexpr uint8_t NO_STREAM_CLIENT = 0xFF;

WebServer server(80);
WebSocketsServer webSocket(81);

uint8_t *cachedFrame = nullptr;
uint8_t *snapshotBuffer = nullptr;
uint16_t detectedSensorPid = 0;
uint32_t cachedFrameId = 0;
unsigned long cachedFrameAt = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastStreamFrame = 0;
uint32_t streamedFrames = 0;
uint8_t streamClient = NO_STREAM_CLIENT;
uint8_t streamFps = DEFAULT_STREAM_FPS;
bool cameraReady = false;
bool modelReady = false;
bool cachedFrameValid = false;
bool operationBusy = false;
bool streamActive = false;

void printMemoryDiagnostics(const char *context) {
  Serial.printf("[%s] free heap=%u, minimum heap=%u, free PSRAM=%u\n",
                context,
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                ESP.getFreePsram());
}

void sendJson(int statusCode, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json", json);
}

void sendJsonError(int statusCode, const char *detail) {
  sendJson(statusCode, "{\"detail\":\"" + String(detail) + "\"}");
}

bool initializeBuffers() {
  cachedFrame = static_cast<uint8_t *>(
      heap_caps_malloc(RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  snapshotBuffer = static_cast<uint8_t *>(
      heap_caps_malloc(RGB888_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (cachedFrame == nullptr || snapshotBuffer == nullptr) {
    Serial.println("Could not allocate inference buffers in PSRAM.");
    if (cachedFrame != nullptr) {
      heap_caps_free(cachedFrame);
      cachedFrame = nullptr;
    }
    if (snapshotBuffer != nullptr) {
      heap_caps_free(snapshotBuffer);
      snapshotBuffer = nullptr;
    }
    return false;
  }

  return true;
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
  config.frame_size = FRAMESIZE_QVGA;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("esp_camera_init() failed with code 0x%x\n", error);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    Serial.println("Could not read the initialized camera sensor.");
    esp_camera_deinit();
    return false;
  }

  detectedSensorPid = sensor->id.PID;
  Serial.printf("Detected sensor PID=0x%04x (expected GC2145=0x%04x)\n",
                detectedSensorPid,
                EXPECTED_SENSOR_PID);
  if (detectedSensorPid != EXPECTED_SENSOR_PID) {
    Serial.println("WARNING: sensor PID differs from the verified GC2145.");
  }

  return true;
}

bool isExpectedFrame(const camera_fb_t *frame) {
  return frame != nullptr && frame->format == PIXFORMAT_RGB565 &&
         frame->width == CAMERA_WIDTH && frame->height == CAMERA_HEIGHT &&
         frame->len == RGB565_BYTES;
}

bool cacheCameraFrame(camera_fb_t **capturedFrame, String &errorMessage) {
  *capturedFrame = nullptr;
  if (!cameraReady) {
    errorMessage = "camera_not_ready";
    return false;
  }
  if (cachedFrame == nullptr) {
    errorMessage = "cache_unavailable";
    return false;
  }

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    errorMessage = "capture_failed";
    return false;
  }
  if (!isExpectedFrame(frame)) {
    Serial.printf("Unexpected framebuffer: %ux%u, format=%d, bytes=%u\n",
                  frame->width,
                  frame->height,
                  frame->format,
                  frame->len);
    esp_camera_fb_return(frame);
    errorMessage = "invalid_frame";
    return false;
  }

  memcpy(cachedFrame, frame->buf, RGB565_BYTES);
  cachedFrameValid = true;
  cachedFrameAt = millis();
  ++cachedFrameId;
  *capturedFrame = frame;
  return true;
}

static int getSnapshotData(size_t offset, size_t length, float *outPtr) {
  size_t pixelIndex = offset * 3U;
  for (size_t outputIndex = 0; outputIndex < length; ++outputIndex) {
    // Espressif's converter stores BGR bytes; Edge Impulse expects packed RGB.
    outPtr[outputIndex] =
        (snapshotBuffer[pixelIndex + 2] << 16) |
        (snapshotBuffer[pixelIndex + 1] << 8) |
        snapshotBuffer[pixelIndex];
    pixelIndex += 3U;
  }
  return 0;
}

bool prepareCachedFrameForInference(String &errorMessage) {
  if (!cachedFrameValid) {
    errorMessage = "no_cached_frame";
    return false;
  }
  if (snapshotBuffer == nullptr) {
    errorMessage = "inference_buffer_unavailable";
    return false;
  }

  if (!fmt2rgb888(cachedFrame, RGB565_BYTES, PIXFORMAT_RGB565, snapshotBuffer)) {
    errorMessage = "rgb_conversion_failed";
    return false;
  }

  const int resizeResult = ei::image::processing::crop_and_interpolate_rgb888(
      snapshotBuffer,
      CAMERA_WIDTH,
      CAMERA_HEIGHT,
      snapshotBuffer,
      EI_CLASSIFIER_INPUT_WIDTH,
      EI_CLASSIFIER_INPUT_HEIGHT);
  if (resizeResult != 0) {
    Serial.printf("Image resize failed with code %d\n", resizeResult);
    errorMessage = "image_resize_failed";
    return false;
  }

  return true;
}

void handleStatus() {
  String json;
  json.reserve(640);
  json += "{";
  json += "\"device\":\"esp32-cam\",";
  json += "\"firmware_version\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"camera_ready\":" + String(cameraReady ? "true" : "false") + ",";
  json += "\"sensor_pid\":\"0x" + String(detectedSensorPid, HEX) + "\",";
  json += "\"model_ready\":" + String(modelReady ? "true" : "false") + ",";
  json += "\"model_version\":\"" + String(MODEL_VERSION) + "\",";
  json += "\"confidence_threshold\":" + String(MIN_CLASSIFICATION_CONFIDENCE, 2) + ",";
  json += "\"frame_cached\":" + String(cachedFrameValid ? "true" : "false") + ",";
  json += "\"frame_id\":" + String(cachedFrameId) + ",";
  json += "\"frame_age_ms\":" +
          String(cachedFrameValid ? millis() - cachedFrameAt : 0) + ",";
  json += "\"operation_busy\":" + String(operationBusy ? "true" : "false") + ",";
  json += "\"wifi_connected\":" +
          String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifi_rssi\":" +
          String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"free_psram\":" + String(ESP.getFreePsram()) + ",";
  json += "\"format\":\"RGB565\",";
  json += "\"resolution\":\"320x240\",";
  json += "\"websocket_port\":81,";
  json += "\"stream_fps\":" + String(streamFps) + ",";
  json += "\"stream_active\":" + String(streamActive ? "true" : "false") + ",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";
  sendJson(200, json);
}

void handleCapture() {
  if (operationBusy) {
    sendJsonError(409, "device_busy");
    return;
  }

  operationBusy = true;
  printMemoryDiagnostics("before capture");

  camera_fb_t *frame = nullptr;
  String errorMessage;
  if (!cacheCameraFrame(&frame, errorMessage)) {
    operationBusy = false;
    sendJsonError(503, errorMessage.c_str());
    return;
  }

  uint8_t *bmpBuffer = nullptr;
  size_t bmpLength = 0;
  const bool converted = frame2bmp(frame, &bmpBuffer, &bmpLength);
  esp_camera_fb_return(frame);

  if (!converted || bmpBuffer == nullptr || bmpLength == 0) {
    if (bmpBuffer != nullptr) {
      free(bmpBuffer);
    }
    operationBusy = false;
    sendJsonError(503, "bmp_conversion_failed");
    return;
  }

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("X-Frame-Id", String(cachedFrameId));
  server.sendHeader("X-Sensor-PID", "0x" + String(detectedSensorPid, HEX));
  server.setContentLength(bmpLength);
  server.send(200, "image/bmp", "");
  server.sendContent(reinterpret_cast<const char *>(bmpBuffer), bmpLength);

  free(bmpBuffer);
  operationBusy = false;
  printMemoryDiagnostics("after capture");
}

void handleClassify() {
  if (operationBusy) {
    sendJsonError(409, "device_busy");
    return;
  }
  if (!modelReady) {
    sendJsonError(503, "model_not_ready");
    return;
  }
  if (!cachedFrameValid) {
    sendJsonError(409, "no_cached_frame");
    return;
  }

  operationBusy = true;
  printMemoryDiagnostics("before inference");

  String errorMessage;
  if (!prepareCachedFrameForInference(errorMessage)) {
    operationBusy = false;
    sendJsonError(503, errorMessage.c_str());
    return;
  }

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = getSnapshotData;

  ei_impulse_result_t result = {};
  const EI_IMPULSE_ERROR inferenceError = run_classifier(&signal, &result, false);
  if (inferenceError != EI_IMPULSE_OK) {
    Serial.printf("run_classifier() failed with code %d\n", inferenceError);
    operationBusy = false;
    sendJsonError(500, "inference_failed");
    return;
  }

  size_t bestIndex = 0;
  float bestConfidence = result.classification[0].value;
  for (size_t index = 1; index < EI_CLASSIFIER_LABEL_COUNT; ++index) {
    if (result.classification[index].value > bestConfidence) {
      bestIndex = index;
      bestConfidence = result.classification[index].value;
    }
  }

  const bool classified = bestConfidence >= MIN_CLASSIFICATION_CONFIDENCE;
  const char *predictedClass = classified
      ? result.classification[bestIndex].label
      : "not_classified";

  String json;
  json.reserve(640);
  json += "{";
  json += "\"status\":\"" + String(classified ? "classified" : "not_classified") + "\",";
  json += "\"predicted_class\":\"" + String(predictedClass) + "\",";
  json += "\"confidence\":" + String(bestConfidence, 6) + ",";
  json += "\"confidence_threshold\":" + String(MIN_CLASSIFICATION_CONFIDENCE, 2) + ",";
  json += "\"frame_id\":" + String(cachedFrameId) + ",";
  json += "\"frame_age_ms\":" + String(millis() - cachedFrameAt) + ",";
  json += "\"scores\":{";
  for (size_t index = 0; index < EI_CLASSIFIER_LABEL_COUNT; ++index) {
    if (index > 0) {
      json += ",";
    }
    json += "\"" + String(result.classification[index].label) + "\":" +
            String(result.classification[index].value, 6);
  }
  json += "},";
  json += "\"timing_ms\":{";
  json += "\"dsp\":" + String(result.timing.dsp) + ",";
  json += "\"classification\":" + String(result.timing.classification) + ",";
  json += "\"anomaly\":" + String(result.timing.anomaly);
  json += "},";
  json += "\"model_version\":\"" + String(MODEL_VERSION) + "\"";
  json += "}";

  operationBusy = false;
  sendJson(200, json);
  printMemoryDiagnostics("after inference");
}

void handleWebSocketEvent(uint8_t client,
                          WStype_t type,
                          uint8_t *payload,
                          size_t length) {
  if (type == WStype_CONNECTED) {
    webSocket.sendTXT(client, "READY");
    return;
  }

  if (type == WStype_DISCONNECTED) {
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
      webSocket.sendTXT(client, "ERROR:FPS_RANGE_1_4");
      return;
    }
    streamFps = static_cast<uint8_t>(requestedFps);
    webSocket.sendTXT(client, "FPS:" + String(streamFps));
  } else {
    webSocket.sendTXT(client, "ERROR:UNKNOWN_COMMAND");
  }
}

void sendStreamFrame() {
  if (operationBusy || !cameraReady) {
    return;
  }

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    webSocket.sendTXT(streamClient, "ERROR:CAPTURE_FAILED");
    return;
  }
  if (!isExpectedFrame(frame)) {
    esp_camera_fb_return(frame);
    webSocket.sendTXT(streamClient, "ERROR:INVALID_FRAME");
    return;
  }

  const size_t frameLength = frame->len;
  const bool sent = webSocket.sendBIN(streamClient, frame->buf, frameLength);
  esp_camera_fb_return(frame);

  if (!sent) {
    streamActive = false;
    return;
  }

  ++streamedFrames;
  if (streamedFrames % 20 == 0) {
    Serial.printf("Streamed %u frames at target %u FPS\n",
                  streamedFrames,
                  streamFps);
  }
}

void startHttpServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200,
                "text/plain; charset=utf-8",
                "ESP32-CAM inference ready. Use GET /status, GET /capture, or POST /classify");
  });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/classify", HTTP_POST, handleClassify);
  server.onNotFound([]() { sendJsonError(404, "endpoint_not_found"); });
  server.begin();
  Serial.println("HTTP server started on port 80.");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_RETRY_MS) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected. IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi unavailable; loop() will retry.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-CAM GC2145 Edge Impulse inference server");
  Serial.printf("PSRAM detected=%s, total=%u bytes\n",
                psramFound() ? "yes" : "no",
                ESP.getPsramSize());
  printMemoryDiagnostics("startup");

  if (!psramFound()) {
    Serial.println("PSRAM is required for camera caching and inference.");
  } else {
    cameraReady = initializeCamera();
    if (initializeBuffers()) {
      run_classifier_init();
      modelReady = true;
    }
  }

  connectWifi();
  startHttpServer();

  webSocket.begin();
  webSocket.onEvent(handleWebSocketEvent);
  Serial.printf("WebSocket server started on port 81 at %u FPS.\n", streamFps);
  printMemoryDiagnostics("ready");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  const unsigned long streamInterval = 1000UL / streamFps;
  if (streamActive && WiFi.status() == WL_CONNECTED && !operationBusy &&
      millis() - lastStreamFrame >= streamInterval) {
    lastStreamFrame = millis();
    sendStreamFrame();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    streamActive = false;
    streamClient = NO_STREAM_CLIENT;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  delay(2);
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "The installed Edge Impulse model is not configured for camera input"
#endif
