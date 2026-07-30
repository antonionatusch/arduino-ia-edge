# Arquitectura

## Componentes

```text
                 red local

Flutter <----> FastAPI / Ubuntu Server
                    |       |
                    |       +---- HTTP/WS ----> ESP32-CAM
                    |
                    +------------ HTTP ------> ESP32 maestro
                                                   |
                                                   +--> rele
                                                        |-- ESP32-CAM
                                                        +-- ventilador
```

FastAPI es la frontera del sistema para aplicaciones cliente. Flutter no debe
acceder directamente a las placas porque sus IP pueden cambiar, sus APIs son
limitadas y no deben exponerse a Internet.

## Responsabilidades

### ESP32 maestro

- Controlar fisicamente el rele en GPIO 26.
- Mantener el horario aunque Ubuntu no este disponible.
- Obtener hora NTP y aplicar el huso fijo `UTC-4`.
- Ofrecer modos manuales para mantenimiento.
- Permanecer apagado cuando el modo automatico no tenga hora valida.

No coordina inferencias ni calcula mayorias.

### ESP32-CAM

- Inicializar el sensor GC2145.
- Entregar estado, capturas y video.
- En una fase posterior, ejecutar el modelo Edge Impulse.
- Reportar predicciones; no decidir el resultado de una ronda completa.

## Horario de operacion

El maestro controla el rele en ventanas horarias para rondas desde las 8:00 AM
hasta las 10:00 PM:

- **Encendido:** `hh:59:30`, treinta segundos antes de cada ronda.
- **Apagado:** `hh:05:20`, despues de la quinta clasificacion.
- **Entre rondas y fuera de horario:** el rele permanece apagado.

La CAM solo recibe energia durante cada ventana de aproximadamente seis
minutos. El resto del tiempo cualquier consulta desde FastAPI o Flutter
devolvera timeout, lo cual es un estado esperado.

### FastAPI

- Ocultar las APIs internas de las placas.
- Aplicar timeouts y reportar dispositivos no disponibles.
- Coordinar las cinco clasificaciones futuras.
- Persistir rondas y calcular el voto mayoritario.
- Retransmitir video y enviar notificaciones.

El horario de energia no depende de FastAPI. La coordinacion de inferencia si
dependera del backend porque requiere historial, reintentos y reglas de voto.

### Flutter

- Mostrar estado, video, capturas e historial.
- Solicitar cambios de modo al backend.
- Recibir notificaciones.

## Flujo automatico futuro

1. A `hh:59:30`, el maestro activa el rele para la siguiente ronda horaria.
2. La CAM arranca, se conecta a Wi-Fi e inicializa sensor y modelo.
3. FastAPI sondea el estado de la CAM hasta que este lista o venza el timeout.
4. FastAPI solicita una clasificacion en cada instante de la ronda.
5. FastAPI conserva cada resultado individual.
6. Tras la quinta muestra calcula el voto mayoritario.
7. FastAPI persiste la ronda y envia la notificacion.
8. A `hh:05:20`, el maestro desactiva el rele hasta la siguiente ronda. La
   ventana de las 10:00 PM termina a las 22:05:20.

## Decisiones de seguridad

- Maestro y backend usan tokens diferentes.
- `WiFiCredentials.h` y `.env` no se versionan.
- La etapa inicial supone una LAN confiable.
- Antes de acceso remoto se debe colocar FastAPI detras de HTTPS y reemplazar
  el token compartido de Flutter por autenticacion de usuarios.
- No se deben abrir los puertos 80/81 de las placas en el router.

## Direcciones de red

Se recomiendan reservas DHCP en lugar de codificar IP estatica en el firmware.
Ubuntu configura las URLs con `MASTER_URL` y `CAMERA_URL`. Esto permite cambiar
el direccionamiento sin volver a cargar los ESP32.
