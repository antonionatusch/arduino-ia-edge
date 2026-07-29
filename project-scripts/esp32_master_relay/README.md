# ESP32 maestro de energia

Firmware para el ESP32 Dev Module que controla, mediante un rele en GPIO 26,
la alimentacion de la ESP32-CAM y del ventilador.

## Comportamiento

- Arranca con el rele apagado.
- En modo `automatic` mantiene el rele apagado hasta tener hora NTP valida.
- Horario de operacion: 8:00 AM a 10:00 PM (hora Bolivia).
- Enciende a las 7:59:30 para precalentar antes del primer ciclo.
- Ejecuta 5 clasificaciones por hora dentro del horario.
- Apaga a las 22:05:20 despues del ultimo ciclo del dia.
- Los modos `manual_on` y `manual_off` permiten diagnosticar el conjunto.
- Si pierde Wi-Fi, el horario automatico sigue usando el reloj ya sincronizado.
- Si reinicia sin poder obtener una hora valida, permanece apagado.

## Preparacion

1. Copiar `WiFiCredentials.example.h` como `WiFiCredentials.h` en esta carpeta.
2. Cambiar `SSID`, `PASSWORD` y `API_TOKEN`.
3. Seleccionar `ESP32 Dev Module` en Arduino IDE.
4. Cargar `esp32_master_relay.ino`.

`WiFiCredentials.h` esta ignorado por Git y no debe versionarse.

## Compilacion por CLI

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 project-scripts/esp32_master_relay
```

El sketch compila sin el archivo de credenciales para facilitar verificaciones,
pero no podra conectarse hasta que se cree dicho archivo.

## Pruebas HTTP directas

```bash
curl http://IP_DEL_MAESTRO/api/v1/status

curl -X POST \
  -H "X-API-Key: TOKEN_DEL_MAESTRO" \
  "http://IP_DEL_MAESTRO/api/v1/mode?value=manual_on"

curl -X POST \
  -H "X-API-Key: TOKEN_DEL_MAESTRO" \
  "http://IP_DEL_MAESTRO/api/v1/mode?value=automatic"
```

La aplicacion final no debe llamar estos endpoints directamente. FastAPI sera
el intermediario entre Flutter y los dispositivos.

## Conexion electrica

GPIO 26 controla la entrada logica del modulo de rele; no debe alimentar el
ventilador ni la ESP32-CAM directamente. El firmware supone un rele activo en
`HIGH`. Si el modulo es activo en `LOW`, cambiar `RELAY_ACTIVE_LOW` a `true`.
