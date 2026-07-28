#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "WiFiCredentials.h"

// ======================================================
// CREDENCIALES WI-FI
// ======================================================

constexpr char WIFI_SSID[] = SSID;
constexpr char WIFI_PASSWORD[] = PASSWORD;

// ======================================================
// PINOUT ESP32-CAM AI THINKER
// ======================================================

constexpr int PWDN_GPIO_NUM  = 32;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM  = 0;
constexpr int SIOD_GPIO_NUM  = 26;
constexpr int SIOC_GPIO_NUM  = 27;

constexpr int Y9_GPIO_NUM    = 35;
constexpr int Y8_GPIO_NUM    = 34;
constexpr int Y7_GPIO_NUM    = 39;
constexpr int Y6_GPIO_NUM    = 36;
constexpr int Y5_GPIO_NUM    = 21;
constexpr int Y4_GPIO_NUM    = 19;
constexpr int Y3_GPIO_NUM    = 18;
constexpr int Y2_GPIO_NUM    = 5;

constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM  = 23;
constexpr int PCLK_GPIO_NUM  = 22;

constexpr int FLASH_LED_PIN = 4;

// ======================================================
// CONFIGURACIÓN
// ======================================================

constexpr unsigned long WIFI_TIMEOUT_MS = 15000;
constexpr unsigned long CAPTURE_INTERVAL_MS = 3000;

bool cameraReady = false;
bool wifiReady = false;
unsigned long lastCaptureMs = 0;

// ======================================================
// LED FLASH
// ======================================================

void setFlash(bool enabled) {
  digitalWrite(FLASH_LED_PIN, enabled ? HIGH : LOW);
}

void blinkFlash(int repetitions, int onMs = 150, int offMs = 150) {
  for (int i = 0; i < repetitions; i++) {
    setFlash(true);
    delay(onMs);

    setFlash(false);
    delay(offMs);
  }
}

// ======================================================
// INFORMACIÓN DEL SISTEMA
// ======================================================

void printSystemInfo() {
  Serial.println();
  Serial.println("====================================");
  Serial.println("   DIAGNÓSTICO ESP32-CAM");
  Serial.println("====================================");

  Serial.printf("Chip: %s\n", ESP.getChipModel());
  Serial.printf("Revisión: %d\n", ESP.getChipRevision());
  Serial.printf("Núcleos: %d\n", ESP.getChipCores());
  Serial.printf("CPU: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("Heap libre: %u bytes\n", ESP.getFreeHeap());

  if (psramFound()) {
    Serial.println("PSRAM: DETECTADA");
    Serial.printf("PSRAM total: %u bytes\n", ESP.getPsramSize());
    Serial.printf("PSRAM libre: %u bytes\n", ESP.getFreePsram());
  } else {
    Serial.println("PSRAM: NO DETECTADA");
  }

  Serial.println("====================================");
}

// ======================================================
// INICIALIZACIÓN DE CÁMARA
// ======================================================

bool initializeCamera() {
  Serial.println();
  Serial.println("[CAM] Configurando cámara...");

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

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;  // 320 x 240
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QQVGA; // 160 x 120
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  const esp_err_t result = esp_camera_init(&config);

  if (result != ESP_OK) {
    Serial.printf(
        "[ERROR] No se pudo inicializar la cámara. Código: 0x%X\n",
        result
    );

    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();

  if (sensor == nullptr) {
    Serial.println("[ERROR] No se pudo obtener la información del sensor.");
    esp_camera_deinit();
    return false;
  }

  Serial.println("[CAM] Cámara inicializada correctamente.");
  Serial.printf("[CAM] PID del sensor: 0x%04X\n", sensor->id.PID);
  Serial.printf("[CAM] Versión: 0x%02X\n", sensor->id.VER);
  Serial.printf("[CAM] MIDH: 0x%02X\n", sensor->id.MIDH);
  Serial.printf("[CAM] MIDL: 0x%02X\n", sensor->id.MIDL);

  /*
   * Ajustes conservadores para diagnóstico.
   * No aplicamos filtros todavía.
   */
  sensor->set_framesize(sensor, FRAMESIZE_QVGA);
  sensor->set_brightness(sensor, 0);
  sensor->set_contrast(sensor, 0);
  sensor->set_saturation(sensor, 0);
  sensor->set_vflip(sensor, 0);
  sensor->set_hmirror(sensor, 0);

  return true;
}

// ======================================================
// PRUEBA DE CAPTURA
// ======================================================

bool captureDiagnosticFrame() {
  if (!cameraReady) {
    return false;
  }

  Serial.println();
  Serial.println("[CAPTURA] Solicitando imagen...");

  const unsigned long startMs = millis();

  camera_fb_t *frame = esp_camera_fb_get();

  if (frame == nullptr) {
    Serial.println("[ERROR] No se pudo obtener el frame buffer.");
    blinkFlash(4, 80, 80);
    return false;
  }

  const unsigned long elapsedMs = millis() - startMs;

  Serial.println("[CAPTURA] Imagen obtenida.");
  Serial.printf("[CAPTURA] Ancho: %u px\n", frame->width);
  Serial.printf("[CAPTURA] Alto: %u px\n", frame->height);
  Serial.printf("[CAPTURA] Tamaño: %u bytes\n", frame->len);
  Serial.printf("[CAPTURA] Formato: %d\n", frame->format);
  Serial.printf("[CAPTURA] Tiempo: %lu ms\n", elapsedMs);
  Serial.printf("[MEMORIA] Heap libre: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEMORIA] PSRAM libre: %u bytes\n", ESP.getFreePsram());

  esp_camera_fb_return(frame);

  blinkFlash(1, 100, 50);

  return true;
}

// ======================================================
// WI-FI
// ======================================================

bool connectWiFi() {
  Serial.println();
  Serial.printf("[WIFI] Conectando a \"%s\"", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs >= WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("[ERROR] Tiempo de conexión Wi-Fi agotado.");
      return false;
    }

    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("[WIFI] Conectado correctamente.");
  Serial.print("[WIFI] Dirección IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("[WIFI] Intensidad RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  return true;
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  pinMode(FLASH_LED_PIN, OUTPUT);
  setFlash(false);

  delay(1000);

  printSystemInfo();

  cameraReady = initializeCamera();

  if (!cameraReady) {
    Serial.println();
    Serial.println("[FALLO] Diagnóstico detenido: cámara no disponible.");

    while (true) {
      blinkFlash(3, 150, 150);
      delay(1500);
    }
  }

  /*
   * Descartamos las primeras capturas, porque el control automático
   * de exposición y balance de blancos puede tardar en estabilizarse.
   */
  Serial.println("[CAM] Descartando frames iniciales...");

  for (int i = 0; i < 3; i++) {
    camera_fb_t *frame = esp_camera_fb_get();

    if (frame != nullptr) {
      esp_camera_fb_return(frame);
    }

    delay(250);
  }

  if (!captureDiagnosticFrame()) {
    Serial.println("[FALLO] La cámara inicializó, pero no pudo capturar.");
  }

  wifiReady = connectWiFi();

  Serial.println();
  Serial.println("====================================");

  if (cameraReady && wifiReady) {
    Serial.println("DIAGNÓSTICO COMPLETADO: CAM_READY");
    blinkFlash(2, 250, 150);
  } else if (cameraReady) {
    Serial.println("CÁMARA LISTA, PERO SIN WI-FI");
    blinkFlash(2, 80, 400);
  }

  Serial.println("====================================");

  lastCaptureMs = millis();
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiReady) {
      Serial.println("[WIFI] Conexión perdida.");
      wifiReady = false;
    }
  } else if (!wifiReady) {
    Serial.println("[WIFI] Conexión recuperada.");
    wifiReady = true;
  }

  if (millis() - lastCaptureMs >= CAPTURE_INTERVAL_MS) {
    lastCaptureMs = millis();
    captureDiagnosticFrame();
  }

  delay(20);
}