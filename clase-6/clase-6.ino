const int PIN_LED_R = 10;
const int PIN_LED_G = 9;
const int PIN_LED_B = 8;

void apagarTodos() {
  digitalWrite(PIN_LED_R, LOW);
  digitalWrite(PIN_LED_G, LOW);
  digitalWrite(PIN_LED_B, LOW);
}

void setup() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  apagarTodos();

  Serial.begin(9600);
}

void loop() {
  if (Serial.available() > 0) {
    String comando = Serial.readStringUntil('\n');
    comando.trim();

    if (comando == "R") {
      apagarTodos();
      digitalWrite(PIN_LED_R, HIGH);
    } else if (comando == "G") {
      apagarTodos();
      digitalWrite(PIN_LED_G, HIGH);
    } else if (comando == "B") {
      apagarTodos();
      digitalWrite(PIN_LED_B, HIGH);
    } else if (comando == "N") {
      apagarTodos();
    }
  }
}