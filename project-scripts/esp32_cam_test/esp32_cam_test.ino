#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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

constexpr int Y9_GPIO_NUM = 35;
constexpr int Y8_GPIO_NUM = 34;
constexpr int Y7_GPIO_NUM = 39;
constexpr int Y6_GPIO_NUM = 36;
constexpr int Y5_GPIO_NUM = 21;
constexpr int Y4_GPIO_NUM = 19;
constexpr int Y3_GPIO_NUM = 18;
constexpr int Y2_GPIO_NUM = 5;

constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM  = 23;
constexpr int PCLK_GPIO_NUM  = 22;

constexpr int FLASH_LED_PIN = 4;

// ======================================================
// CONFIGURACIÓN
// ======================================================

constexpr unsigned long WIFI_TIMEOUT_MS = 15000;

// Tamaño de cada bloque enviado por TCP.
constexpr size_t HTTP_CHUNK_SIZE = 4096;

// Una imagen QVGA RGB565 debe ocupar:
// 320 × 240 × 2 = 153600 bytes.
constexpr size_t EXPECTED_FRAME_SIZE = 320UL * 240UL * 2UL;

// ======================================================
// ESTADO GLOBAL
// ======================================================

bool cameraReady = false;
bool wifiReady = false;
bool httpServerReady = false;

WebServer server(80);


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
    Serial.printf(
        "PSRAM total: %u bytes\n",
        ESP.getPsramSize()
    );
    Serial.printf(
        "PSRAM libre: %u bytes\n",
        ESP.getFreePsram()
    );
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

  /*
   * El sensor GC2145 no soporta JPEG por hardware.
   * Trabajamos con RGB565:
   *
   * 16 bits por píxel = 2 bytes por píxel.
   */
  config.pixel_format = PIXFORMAT_RGB565;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;  // 320 × 240
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QQVGA; // 160 × 120
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  const esp_err_t result = esp_camera_init(&config);

  if (result != ESP_OK) {
    Serial.printf(
        "[ERROR] No se pudo inicializar la cámara. "
        "Código: 0x%X\n",
        result
    );

    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();

  if (sensor == nullptr) {
    Serial.println(
        "[ERROR] No se pudo obtener la información "
        "del sensor."
    );

    esp_camera_deinit();
    return false;
  }

  Serial.println("[CAM] Cámara inicializada correctamente.");
  Serial.printf(
      "[CAM] PID del sensor: 0x%04X\n",
      sensor->id.PID
  );
  Serial.printf(
      "[CAM] Versión: 0x%02X\n",
      sensor->id.VER
  );
  Serial.printf(
      "[CAM] MIDH: 0x%02X\n",
      sensor->id.MIDH
  );
  Serial.printf(
      "[CAM] MIDL: 0x%02X\n",
      sensor->id.MIDL
  );

  /*
   * Ajustes neutros para la primera prueba visual.
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
// DESCARTE DE FRAMES INICIALES
// ======================================================

void discardInitialFrames(int frameCount) {
  Serial.printf(
      "[CAM] Descartando %d frames iniciales...\n",
      frameCount
  );

  for (int i = 0; i < frameCount; i++) {
    camera_fb_t *frame = esp_camera_fb_get();

    if (frame != nullptr) {
      esp_camera_fb_return(frame);
    }

    delay(250);
  }

  Serial.println("[CAM] Frames iniciales descartados.");
}

// ======================================================
// PRUEBA LOCAL DE CAPTURA
// ======================================================

bool captureDiagnosticFrame() {
  if (!cameraReady) {
    return false;
  }

  Serial.println();
  Serial.println("[CAPTURA] Solicitando imagen...");

  const unsigned long startUs = micros();

  camera_fb_t *frame = esp_camera_fb_get();

  if (frame == nullptr) {
    Serial.println(
        "[ERROR] No se pudo obtener el frame buffer."
    );

    
    return false;
  }

  const unsigned long elapsedUs = micros() - startUs;

  Serial.println("[CAPTURA] Imagen obtenida.");
  Serial.printf(
      "[CAPTURA] Ancho: %u px\n",
      frame->width
  );
  Serial.printf(
      "[CAPTURA] Alto: %u px\n",
      frame->height
  );
  Serial.printf(
      "[CAPTURA] Tamaño: %u bytes\n",
      frame->len
  );
  Serial.printf(
      "[CAPTURA] Formato: %d\n",
      frame->format
  );
  Serial.printf(
      "[CAPTURA] Tiempo: %lu us (%.2f ms)\n",
      elapsedUs,
      elapsedUs / 1000.0
  );

  if (frame->len == EXPECTED_FRAME_SIZE) {
    Serial.println(
        "[CAPTURA] Tamaño RGB565 correcto."
    );
  } else {
    Serial.printf(
        "[CAPTURA][ADVERTENCIA] Se esperaban %u bytes.\n",
        EXPECTED_FRAME_SIZE
    );
  }

  Serial.printf(
      "[MEMORIA] Heap libre: %u bytes\n",
      ESP.getFreeHeap()
  );
  Serial.printf(
      "[MEMORIA] PSRAM libre: %u bytes\n",
      ESP.getFreePsram()
  );

  esp_camera_fb_return(frame);

  

  return true;
}

// ======================================================
// WI-FI
// ======================================================

bool connectWiFi() {
  Serial.println();
  Serial.printf(
      "[WIFI] Conectando a \"%s\"",
      WIFI_SSID
  );

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs >= WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println(
          "[ERROR] Tiempo de conexión Wi-Fi agotado."
      );

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
// RUTA HTTP: PÁGINA PRINCIPAL
// ======================================================

void handleRoot() {
  String response;

  response.reserve(300);

  response += "ESP32-CAM lista\n";
  response += "Sensor: GC2145\n";
  response += "Formato: RGB565\n";
  response += "Resolucion: 320x240\n";
  response += "Bytes esperados: ";
  response += String(EXPECTED_FRAME_SIZE);
  response += "\n";
  response += "Captura: /capture\n";
  response += "IP: ";
  response += WiFi.localIP().toString();
  response += "\n";
  response += "RSSI: ";
  response += String(WiFi.RSSI());
  response += " dBm\n";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", response);
}

// ======================================================
// RUTA HTTP: CAPTURA RGB565
// ======================================================

void handleCapture() {
  if (!cameraReady) {
    server.send(
        503,
        "text/plain; charset=utf-8",
        "Camara no disponible"
    );

    return;
  }

  Serial.println();
  Serial.println(
      "[HTTP] Solicitud de captura recibida."
  );

  const unsigned long requestStartUs = micros();
  const unsigned long captureStartUs = micros();

  camera_fb_t *frame = esp_camera_fb_get();

  const unsigned long captureElapsedUs =
      micros() - captureStartUs;

  if (frame == nullptr) {
    Serial.println(
        "[HTTP][ERROR] No se pudo obtener la captura."
    );

    server.send(
        500,
        "text/plain; charset=utf-8",
        "No se pudo obtener la captura"
    );

    
    return;
  }

  /*
   * Guardamos los metadatos antes de devolver el buffer.
   */
  const size_t frameLength = frame->len;
  const size_t frameWidth = frame->width;
  const size_t frameHeight = frame->height;
  const pixformat_t frameFormat = frame->format;

  Serial.printf(
      "[HTTP] Dimensiones: %u x %u\n",
      frameWidth,
      frameHeight
  );
  Serial.printf(
      "[HTTP] Tamaño: %u bytes\n",
      frameLength
  );
  Serial.printf(
      "[HTTP] Formato: %d\n",
      frameFormat
  );
  Serial.printf(
      "[HTTP] Obtención del buffer: %lu us\n",
      captureElapsedUs
  );

  if (frameLength != EXPECTED_FRAME_SIZE) {
    Serial.printf(
        "[HTTP][ADVERTENCIA] Se esperaban %u bytes.\n",
        EXPECTED_FRAME_SIZE
    );
  }

  /*
   * Cabeceras que utilizará el script Python para interpretar
   * correctamente la matriz de píxeles.
   */
  server.sendHeader(
      "X-Image-Width",
      String(frameWidth)
  );
  server.sendHeader(
      "X-Image-Height",
      String(frameHeight)
  );
  server.sendHeader(
      "X-Pixel-Format",
      "RGB565"
  );
  server.sendHeader(
      "X-Byte-Order",
      "sensor-native"
  );
  server.sendHeader(
      "Cache-Control",
      "no-store"
  );
  server.sendHeader(
      "Connection",
      "close"
  );

  /*
   * Informamos el tamaño exacto del cuerpo HTTP.
   */
  server.setContentLength(frameLength);

  /*
   * Envía el estado y las cabeceras.
   * El contenido binario se escribe inmediatamente después
   * mediante el WiFiClient asociado a esta solicitud.
   */
  server.send(
      200,
      "application/octet-stream",
      ""
  );

  WiFiClient client = server.client();

  client.setTimeout(5000);

  const unsigned long transferStartUs = micros();

  size_t totalSent = 0;

  while (
      totalSent < frameLength &&
      client.connected()
  ) {
    const size_t remaining =
        frameLength - totalSent;

    const size_t chunkSize =
        remaining > HTTP_CHUNK_SIZE
            ? HTTP_CHUNK_SIZE
            : remaining;

    const size_t sent = client.write(
        frame->buf + totalSent,
        chunkSize
    );

    if (sent == 0) {
      Serial.println(
          "[HTTP][ERROR] El cliente dejó de aceptar datos."
      );

      break;
    }

    totalSent += sent;

    /*
     * Permite que las tareas internas de red y del sistema
     * sigan ejecutándose durante la transferencia.
     */
    yield();
  }

  const unsigned long transferElapsedUs =
      micros() - transferStartUs;

  /*
   * El framebuffer ya no se utiliza después de esta llamada.
   */
  esp_camera_fb_return(frame);
  frame = nullptr;

  const unsigned long requestElapsedUs =
      micros() - requestStartUs;

  Serial.printf(
      "[HTTP] Transferencia: %u de %u bytes.\n",
      totalSent,
      frameLength
  );
  Serial.printf(
      "[HTTP] Tiempo de transferencia: %.2f ms\n",
      transferElapsedUs / 1000.0
  );
  Serial.printf(
      "[HTTP] Tiempo total de solicitud: %.2f ms\n",
      requestElapsedUs / 1000.0
  );

  Serial.printf(
      "[MEMORIA] Heap libre: %u bytes\n",
      ESP.getFreeHeap()
  );
  Serial.printf(
      "[MEMORIA] PSRAM libre: %u bytes\n",
      ESP.getFreePsram()
  );

  if (totalSent == frameLength) {
    Serial.println(
        "[HTTP] Captura enviada correctamente."
    );

    
  } else {
    Serial.println(
        "[HTTP][ERROR] Transferencia incompleta."
    );

    
  }

  client.stop();
}

// ======================================================
// RUTA HTTP NO ENCONTRADA
// ======================================================

void handleNotFound() {
  String response;

  response += "Ruta no encontrada\n";
  response += "Rutas disponibles:\n";
  response += "  GET /\n";
  response += "  GET /capture\n";

  server.send(
      404,
      "text/plain; charset=utf-8",
      response
  );
}

// ======================================================
// INICIALIZACIÓN DEL SERVIDOR HTTP
// ======================================================

void startHttpServer() {
  if (httpServerReady) {
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.onNotFound(handleNotFound);

  server.begin();

  httpServerReady = true;

  Serial.println();
  Serial.println("[HTTP] Servidor iniciado.");

  Serial.print("[HTTP] Estado: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");

  Serial.print("[HTTP] Captura: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/capture");
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  delay(1000);

  printSystemInfo();

  cameraReady = initializeCamera();

  if (!cameraReady) {
    Serial.println();
    Serial.println(
        "[FALLO] Diagnóstico detenido: "
        "cámara no disponible."
    );

    while (true) {
      delay(1500);
    }
  }

  /*
   * El control automático de exposición y balance de blancos
   * necesita algunos frames para estabilizarse.
   */
  discardInitialFrames(3);

  if (!captureDiagnosticFrame()) {
    Serial.println(
        "[FALLO] La cámara inicializó, "
        "pero no pudo capturar."
    );
  }

  wifiReady = connectWiFi();

  if (wifiReady) {
    startHttpServer();
  }

  Serial.println();
  Serial.println("====================================");

  if (cameraReady && wifiReady && httpServerReady) {
    Serial.println(
        "DIAGNÓSTICO COMPLETADO: CAM_READY"
    );

    
  } else if (cameraReady) {
    Serial.println(
        "CÁMARA LISTA, PERO SIN WI-FI"
    );

    
  }

  Serial.println("====================================");
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  const wl_status_t currentWiFiStatus = WiFi.status();

  if (currentWiFiStatus != WL_CONNECTED) {
    if (wifiReady) {
      Serial.println("[WIFI] Conexión perdida.");
      wifiReady = false;
    }
  } else {
    if (!wifiReady) {
      Serial.println("[WIFI] Conexión recuperada.");

      wifiReady = true;

      Serial.print("[WIFI] Dirección IP actual: ");
      Serial.println(WiFi.localIP());
    }

    if (httpServerReady) {
      server.handleClient();
    }
  }

  delay(2);
}