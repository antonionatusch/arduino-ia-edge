#include <WiFi.h>
#include <WebSocketsServer.h>

const char* SSID = "RED NATUSCH";

const char* PASSWORD = "26022004";
WebSocketsServer ws(81);

void onWsEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED) {
    Serial.println("Cliente conectado!");
  }
  if (type == WStype_DISCONNECTED) {
    Serial.println("Cliente desconectado.");
  }
  if (type == WStype_TEXT) {
    String mensaje = String((char*)payload);
    mensaje.trim();
    Serial.println("Recibido: " + mensaje);

    if (mensaje == "hola") {
      ws.sendTXT(client, "hola python");
    }
  }
}

void setup() {
  Serial.begin(9600);

  WiFi.begin(SSID, PASSWORD);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  ws.begin();
  ws.onEvent(onWsEvent);
  Serial.println("WebSocket server iniciado en puerto 81");
}

void loop() {
  ws.loop();
}