# ESP32-CAM inference

Firmware for the GC2145 ESP32-CAM that preserves HTTP capture and WebSocket
streaming while adding Edge Impulse classification of the last HTTP capture.

## Requirements

- Board: `ESP32 Dev Module`
- ESP32 Arduino core: `3.3.10`
- PSRAM: enabled
- Partition scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`
- Edge Impulse library: `pet-feeder-monitor_inferencing`

Copy `WiFiCredentials.example.h` to `WiFiCredentials.h` and enter the local
Wi-Fi credentials before uploading. The real credentials file is ignored by
Git.

## HTTP API

- `GET /status`: camera, model, network, memory and cached-frame state.
- `GET /capture`: captures and caches one RGB565 frame, then returns that exact
  frame as BMP.
- `POST /classify`: classifies only the last frame stored by `/capture`.

`/classify` returns `not_classified` when no model score reaches the configured
minimum confidence. This is separate from `unknown`, which is a trained model
class.

## WebSocket

Connect to port `81`. Commands are `START`, `PAUSE`, `STOP`, and `FPS:1` through
`FPS:4`. Stream frames do not replace the frame cached by `GET /capture`.
