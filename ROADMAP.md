# Roadmap del sistema de monitoreo

## Objetivo

Construir un sistema que encienda periodicamente una ESP32-CAM y un
ventilador, observe el plato de un alimentador, obtenga cinco clasificaciones
y notifique a una aplicacion Flutter el resultado mayoritario.

El sistema esta dividido en cuatro componentes:

- ESP32 maestro: energia, horario y modo manual.
- ESP32-CAM: captura, video e inferencia Edge Impulse.
- Ubuntu Server: API, coordinacion, persistencia y notificaciones.
- Flutter: visualizacion y control del usuario.

## Horario de operacion

El sistema ejecuta rondas horarias desde las **8:00 AM hasta las 10:00 PM** en
hora de Bolivia (`UTC-4`). El ESP32 maestro enciende el rele treinta segundos
antes de cada ronda y lo apaga despues de la quinta clasificacion. Entre rondas
el rele permanece apagado.

Cada hora dentro del horario de operacion se ejecuta un ciclo:

| Instante | Accion |
|---|---|
| `hh:59:30` | El maestro enciende CAM y ventilador |
| `hh:00:15` | Clasificacion 1 |
| `hh:01:15` | Clasificacion 2 |
| `hh:02:15` | Clasificacion 3 |
| `hh:03:15` | Clasificacion 4 |
| `hh:04:15` | Clasificacion 5 |
| `hh:05:20` | El maestro apaga CAM y ventilador |

El intervalo previo a la primera clasificacion permite arrancar la CAM. El
backend debera comprobar ademas que `/status` indique que esta lista. Si la CAM
arranca tarde, la ronda se marcara incompleta en lugar de inventar resultados.

## Fase 1: control y diagnostico

Estado: implementacion inicial disponible.

- [x] Firmware separado para el ESP32 maestro.
- [x] Arranque seguro con rele apagado.
- [x] Sincronizacion NTP en hora boliviana.
- [x] Ciclo horario autonomo `hh:59:30` a `hh:05:20`, para rondas de 8:00 AM a 10:00 PM.
- [x] Modos `automatic`, `manual_on` y `manual_off`.
- [x] API HTTP del maestro protegida por token.
- [x] Reconexion Wi-Fi sin detener el control principal.
- [x] Backend FastAPI independiente del dataset y del modelo.
- [x] Proxy de estado/control del maestro.
- [x] Proxy de estado y captura puntual de la CAM.
- [x] Pruebas unitarias del backend con dispositivos simulados.
- [ ] Validar el nivel electrico real del rele sobre el hardware.
- [ ] Reservar IP por DHCP para ambas placas.
- [ ] Instalar FastAPI como servicio de Ubuntu Server.
- [ ] Ejecutar una prueba continua de al menos 24 horas.

## Fase 2: modelo e inferencia

Dependencia: dataset terminado y modelo Edge Impulse exportado para ESP32.

- [ ] Completar y revisar el balance del dataset.
- [ ] Separar entrenamiento, validacion y prueba sin filtrar escenas repetidas.
- [ ] Entrenar MobileNetV1 `96x96` INT8.
- [ ] Medir precision por clase y matriz de confusion.
- [ ] Medir RAM, PSRAM, flash y duracion de inferencia en la CAM real.
- [ ] Integrar el modelo en un nuevo sketch sin eliminar el servidor de estado.
- [ ] Exponer resultado, confianza, tiempo de inferencia y version del modelo.
- [ ] Rechazar o marcar predicciones por debajo de un umbral acordado.

## Fase 3: rondas y voto mayoritario

- [ ] Crear una ronda identificada por hora en FastAPI.
- [ ] Esperar la disponibilidad real de la CAM despues del encendido.
- [ ] Solicitar las cinco clasificaciones en los instantes acordados.
- [ ] Guardar cada clase, confianza, fecha y fotografia asociada.
- [ ] Calcular mayoria solamente con resultados validos.
- [ ] Definir desempate y minimo de muestras validas.
- [ ] Marcar rondas `completed`, `partial`, `failed` o `cancelled`.
- [ ] Evitar duplicados si Ubuntu se reinicia durante una ronda.

Propuesta inicial para discutir: exigir al menos tres resultados validos; en
caso de empate o confianza insuficiente, producir `unknown`.

## Fase 4: video y Flutter

- [ ] Crear el proyecto Flutter.
- [ ] Mostrar conectividad, modo, rele y proximo ciclo.
- [ ] Permitir control manual con confirmacion visual.
- [ ] Implementar puente WebSocket RGB565 entre CAM, Ubuntu y Flutter.
- [ ] Convertir el stream en Ubuntu si el dispositivo movil no puede consumir
  RGB565 eficientemente.
- [ ] Mostrar la ultima captura aunque la CAM este apagada.
- [ ] Mostrar historial de rondas y las cinco predicciones individuales.

## Fase 5: notificaciones y produccion

- [ ] Integrar Firebase Cloud Messaging.
- [ ] Notificar el resultado mayoritario y fallos del dispositivo.
- [ ] Incorporar usuarios y autorizacion en FastAPI.
- [ ] Servir la API exclusivamente mediante HTTPS.
- [ ] Guardar secretos fuera del repositorio.
- [ ] Configurar logs rotativos, metricas y alertas.
- [ ] Definir retencion y respaldo de capturas/resultados.
- [ ] Documentar actualizacion y recuperacion de cada dispositivo.

## Criterios de aceptacion finales

- Un reinicio sin NTP no enciende accidentalmente el rele.
- El horario funciona temporalmente aunque Ubuntu pierda conectividad.
- Flutter nunca necesita conocer la IP ni el token interno de un ESP32.
- Cada notificacion puede rastrearse hasta sus clasificaciones originales.
- Una CAM apagada por horario se reporta como esperada, no como inferencia
  fallida.
- Ninguna credencial real queda versionada.
