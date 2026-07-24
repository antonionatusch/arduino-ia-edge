const int PIN_LED = 8;

void setup() {
  Serial.begin(9600);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
}

void loop() {
  if (Serial.available() > 0) {
    String linea = Serial.readStringUntil('\n');
    linea.trim();

    
    } else if (linea == "0") {
      digitalWrite(PIN_LED, LOW);
    }
  }
}