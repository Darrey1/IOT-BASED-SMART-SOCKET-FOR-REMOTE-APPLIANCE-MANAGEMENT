# IoT Smart Socket

An ESP32-based smart socket for remote appliance management, built on the Nexalware IoT platform. It switches a mains load over MQTT, runs timed schedules from local storage, and keeps running those schedules when the network is down.

Final year project, B.Eng. Electrical and Electronics Engineering (Telecommunications), Federal University of Technology Minna.

---

## What it does

- Switches a mains appliance on and off remotely from a React Native mobile app
- Runs up to 5 independent schedules, each with a full on and off datetime
- Stores schedules in EEPROM and reads time from an onboard RTC, so schedules keep firing with no internet connection
- Syncs schedules with the server once MQTT reconnects, so state survives a power cut or reset
- Authenticates to the broker with per-device credentials, scoped to its own topics
- Shows live status on an I2C LCD and a four-LED indicator panel

## Why it is built this way

In the environment this was designed for, power and internet are both unreliable. A socket that stops honouring its schedule because the router is down, or that forgets every schedule after a power cut, is not useful.

So the device does not depend on the cloud to do its job. Schedules live on the device. Time comes from a battery-backed RTC rather than an NTP server. The cloud is how you change a schedule, not how the schedule runs.

---

## Architecture

```
┌────────────────┐      ┌──────────────────────┐      ┌─────────────────┐
│  ESP32-WROOM   │ MQTT │  mqtt.nexalware.com  │      │  Nexalware API  │
│  Smart Socket  │◄────►│  Mosquitto + go-auth │◄────►│                 │
└────────────────┘      └──────────────────────┘      └────────┬────────┘
        │                          │                           │
   ┌────┴─────┐              credentials              ┌────────┴────────┐
   │  EEPROM  │              validated                │   PostgreSQL    │
   │  + RTC   │              against PG               │   + Redis       │
   └──────────┘                                       └────────┬────────┘
  schedules run                                                │
  with no network                                    ┌─────────┴────────┐
                                                     │  React Native    │
                                                     │   mobile app     │
                                                     └──────────────────┘
```

**Firmware.** ESP32-WROOM-32D in C++ (Arduino framework). Handles Wi-Fi, MQTT session lifecycle, relay control, schedule evaluation, EEPROM persistence, LCD and LED status output.

**Broker.** Mosquitto running the `mosquitto-go-auth` plugin, which validates device credentials against PostgreSQL on every connect. No credentials are managed by hand, and a device can only publish and subscribe inside its own topic namespace.

**Platform.** The Nexalware API holds the device registry, issues MQTT credentials at provisioning, and stores the server copy of each device's schedules.

**App.** React Native client for control, scheduling and live device status.

---

## Hardware

| Component | Connection | Purpose |
|---|---|---|
| ESP32-WROOM-32D | | Wi-Fi, MQTT client, control logic |
| Relay module | GPIO18 via BC547 driver | Mains switching |
| DS1307 RTC | I2C (SDA 21, SCL 22) | Battery-backed timekeeping |
| 16x2 LCD | I2C (shared bus) | Status, time, connection state |
| Blue LED | GPIO15 | Power |
| Green LED | GPIO4 | Wi-Fi connected |
| Yellow LED | GPIO5 | Cloud (MQTT) connected |
| Red LED | GPIO18 | Relay energised |

### Relay driver

The relay module needs 5V to de-energise reliably, but an ESP32 GPIO only sources 3.3V. Driving it directly leaves the relay latched on at boot regardless of the pin state.

The fix is a BC547 NPN transistor stage: 1kΩ on the base from the GPIO, 10kΩ pull-up on the relay input, and the coil switched on the 5V rail. The ESP32 then controls the relay without having to supply its coil voltage.

> **Safety.** This switches mains voltage. Keep the mains and logic sides properly isolated and do not work on it live. If you are not confident with mains wiring, bench test against a low voltage load first.

---

## Scheduling

Five slots, each storing:

| Field | Meaning |
|---|---|
| `onTimestamp` | Unix time at which to switch on |
| `offTimestamp` | Unix time at which to switch off |
| `manualOverride` | Set when the user pressed OFF during an active window |
| `label` | Optional name, for example "Morning" |

Timestamps are full datetimes rather than clock times, so a schedule is a specific event rather than a daily repeat.

**Manual override.** If you press OFF while a schedule window is active, the slot is marked overridden and the relay will not re-fire on the next evaluation. The off time still runs as normal, so the slot clears itself cleanly when the window ends.

**Boot behaviour.** On power-up the device loads schedules from EEPROM immediately and begins evaluating them against the RTC. It does not wait for Wi-Fi or MQTT. Once MQTT connects it requests a sync from the server, so a schedule added while the device was offline arrives on reconnect.

Slot state is reported as Pending, Active, Expired or Cancelled.

---

## Commands

| Command | Direction | Effect |
|---|---|---|
| `ON` / `OFF` | app to device | Immediate relay control |
| `STATUS` | app to device | Request current state |
| `SET_SCHEDULE` | app to device | Write a slot, persisted to EEPROM |
| `CANCEL_SCHEDULE` | app to device | Clear a slot, on device and server |
| `SYNC` | device to server | Request the server copy of all slots |

---

## Getting started

### Prerequisites

- Arduino IDE or PlatformIO with the ESP32 board package
- A Nexalware account and a registered device, which issues the device ID and MQTT credentials
- Libraries: `PubSubClient`, `Wire`, `LiquidCrystal_I2C`, `RTClib`, `EEPROM`

### Configure

Copy the example config and fill in your own values. Never commit real credentials.

```c
#define WIFI_SSID       "your-network"
#define WIFI_PASSWORD   "your-password"
#define MQTT_HOST       "mqtt.nexalware.com"
#define MQTT_PORT       1883
#define DEVICE_ID       "nxw_dev_xxxxxxxxxxxx"
#define MQTT_USERNAME   "d_xxxxxxxxxxxx"
#define MQTT_PASSWORD   "issued-at-provisioning"
```

Credentials are issued when you register the device on the platform. Regenerating a device password keeps the same username, so only one firmware line needs updating.

### Flash

Select ESP32 Dev Module, set the correct port, and upload. The LCD reports Wi-Fi and MQTT connection progress directly, so no serial monitor is needed once it is running.

---

## Device provisioning and security

Devices are registered on the platform before they can connect. Registration issues a device ID and MQTT credentials, which Mosquitto validates against PostgreSQL through the go-auth plugin on every connection attempt.

Each device carries a status of `ACTIVE`, `DISABLED` or `REVOKED`. Disabled is a temporary state the owner controls; revoked is a permanent security action. An unregistered or revoked device cannot connect at all, and a connected device cannot reach another device's topics.

---

## Project status

Working hardware, built and defended as a final year project, running against the live Nexalware platform.

Not yet implemented:

- OTA firmware updates
- Power and current metering
- Repeating schedules, since slots are single events
- More than five schedule slots
- TLS on the MQTT connection

---
