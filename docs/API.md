# Contratos HTTP iniciales

## Autenticacion

Los endpoints del ESP32 maestro que modifican estado requieren:

```text
X-API-Key: token-configurado-en-WiFiCredentials.h
```

Todos los endpoints `/api/v1/*` de FastAPI requieren el token configurado en
`BACKEND_API_TOKEN`. `/health` es la unica excepcion.

## ESP32 maestro

Base de ejemplo: `http://192.168.0.10`.

### `GET /api/v1/status`

No modifica estado y puede consultarse directamente para diagnostico local.

```json
{
  "device": "esp32-master",
  "mode": "automatic",
  "relay_enabled": false,
  "time_synchronized": true,
  "local_time": "2026-07-29T08:20:00",
  "wifi_connected": true,
  "wifi_rssi": -55,
  "ip": "192.168.0.10",
  "uptime_ms": 120000,
  "schedule": {
    "on": "hh:59:30",
    "off": "hh:05:20"
  }
}
```

### `POST /api/v1/mode?value=MODE`

Valores permitidos:

- `automatic`
- `manual_on`
- `manual_off`

Devuelve el mismo documento de estado. Un modo incorrecto devuelve HTTP 400.

### `POST /api/v1/debug/relay?enabled=BOOLEAN`

Acepta `true`, `false`, `1` o `0`. La operacion cambia tambien el modo a
`manual_on` o `manual_off`, evitando que el horario revierta inmediatamente la
prueba. Para finalizar el diagnostico se debe restaurar `automatic`.

## FastAPI

Base de ejemplo: `http://ubuntu-server:8000`.

### `GET /health`

Comprueba solo el proceso de FastAPI:

```json
{"status":"ok"}
```

No implica que las placas esten disponibles.

### `GET /api/v1/system/status`

Consulta ambos dispositivos en paralelo. Que la CAM no responda puede ser
normal fuera del periodo activo.

```json
{
  "master": {
    "available": true,
    "data": {
      "device": "esp32-master"
    }
  },
  "camera": {
    "available": false,
    "error": "timeout"
  }
}
```

### `GET /api/v1/master/status`

Retransmite el estado del maestro.

### `POST /api/v1/master/mode`

```json
{"mode":"automatic"}
```

También acepta `manual_on` y `manual_off`.

### `POST /api/v1/master/debug/relay`

```json
{"enabled":true}
```

### `GET /api/v1/camera/status`

Retransmite el endpoint `/status` del sketch
`dataset_capture_server.ino` actualmente cargado.

### `GET /api/v1/camera/capture`

Retransmite la captura BMP actual. Usa `Cache-Control: no-store`.

## Errores del backend

- HTTP 401: token ausente o incorrecto.
- HTTP 422: cuerpo JSON invalido.
- HTTP 502: dispositivo inaccesible o respuesta invalida.
- HTTP 504: timeout al consultar un dispositivo.

FastAPI publica el contrato OpenAPI interactivo en `/docs` cuando esta en
ejecucion.
