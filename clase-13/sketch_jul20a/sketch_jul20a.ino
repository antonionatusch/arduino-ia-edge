/*
  ESP32 (38 pines, generico) recibe por USB serial "1" (reconocida) o
  "0" (no reconocida), y enciende el LED correspondiente.
  Funciona igual en Arduino Uno/Mega, solo cambia el puerto en Python.
*/

const int PIN_LED_SI = 25;  // se enciende si la persona SI es reconocida
const int PIN_LED_NO = 26;  // se enciende si la persona NO es reconocida

void setup() {
  Serial.begin(9600);
  pinMode(PIN_LED_SI, OUTPUT);
  pinMode(PIN_LED_NO, OUTPUT);
  digitalWrite(PIN_LED_SI, LOW);
  digitalWrite(PIN_LED_NO, LOW);
}

void loop() {
  if (Serial.available() > 0) {
    String linea = Serial.readStringUntil('\n');
    linea.trim();

    if (linea == "1") {
      digitalWrite(PIN_LED_SI, HIGH);
      digitalWrite(PIN_LED_NO, LOW);
    } else if (linea == "0") {
      digitalWrite(PIN_LED_SI, LOW);
      digitalWrite(PIN_LED_NO, HIGH);
    }
  }
}