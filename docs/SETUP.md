# Setup guide

How to deploy your own copy: MQTT broker, then backend, then website, then firmware.

## 1. MQTT broker (HiveMQ Cloud)

Create a free HiveMQ Cloud cluster and note its host (`xxxx.s1.eu.hivemq.cloud`). Create three credentials:

| Credential | Permission | Used by |
| --- | --- | --- |
| Device | Publish and Subscribe | The ESP32 (`secrets.h`) |
| Viewer | **Subscribe only** | The public dashboard (`VIEWER_MQTT` in `index.html`) |
| Admin | Publish and Subscribe | Handed to logged-in admins by the Worker (`MQTT_ADMIN_USER` / `MQTT_ADMIN_PASS` secrets) |

The Viewer login is visible in the page source on purpose. Make sure it is **subscribe-only**, and
give it a password you don't use anywhere else.

## 2. Backend (Cloudflare Worker)

1. Create a Worker and paste in `worker/hydro-log-worker.js`, then deploy.
2. **Bindings** (Settings → Bindings):
   - KV namespace, variable `LOGS`
   - D1 database, variable `DB` (tables are created automatically on first use)
3. **Secrets** (Settings → Variables and Secrets, type *Secret*):
   - `LOG_KEY`: a random string. The firmware's `LOG_API_KEY` must match it.
   - `SETUP_KEY`: a long random string, needed to create or reset admin accounts
   - `MQTT_ADMIN_USER`, `MQTT_ADMIN_PASS`: the Admin HiveMQ credential
4. Test: `<worker URL>/logs` returns `[]`, and `<worker URL>/auth/me` returns "Not logged in or session expired".

## 3. Website

1. In `website/index.html`, set `BROKER_URL`, `VIEWER_MQTT` and `API_BASE` (near the top of the script).
2. In `website/admin-setup.html`, set `API_BASE` to the same Worker URL (no trailing slash).
3. Upload the whole `website` folder as one static-site deployment. Every upload replaces the whole
   site, so always include both files.
4. Open `admin-setup.html`, enter the `SETUP_KEY`, and create your first admin account.

## 4. Firmware (ESP32)

1. Copy `firmware/hydro_vOTA/secrets.example.h` to `secrets.h` in the same folder and fill it in.
2. Arduino IDE settings:
   - Board package: **esp32** by Espressif. Library: **PubSubClient** by Nick O'Leary.
   - Board: *ESP32-WROOM-DA Module* (or *ESP32 Dev Module*)
   - Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA)**, or any scheme "with OTA"
   - Serial Monitor: 115200 baud
3. Check the pin wiring in the `YOUR SETTINGS` block of `hydro_vOTA.ino`.
4. Upload over USB for the first install. After that, use either:
   - **Local network:** Tools → Port → `hydro-esp32 at 192.168.x.x` (asks for `OTA_PASSWORD`)
   - **Dashboard:** Sketch → Export Compiled Binary, drop the `.ino.bin` into the Admin tab, then press *Install*
5. Change `FW_VERSION` with every release so the dashboard shows which version is running.

## MQTT topics (summary)

| Topic | Direction | Payload |
| --- | --- | --- |
| `hydroponics/led1..4/set`, `pump/set` | to device | `1` / `0` (manual mode only) |
| `hydroponics/led1..4/state`, `pump/state` | from device | `1` / `0` (retained) |
| `hydroponics/mode/set` / `state` | both | `auto` / `manual` |
| `hydroponics/schedule/set` / `state` | both | light hours on, e.g. `12.5` |
| `hydroponics/light_start/set` / `state` | both | `HH:MM` (KSA) |
| `hydroponics/pump_schedule/set` / `state` | both | `on,off` minutes, e.g. `15,15` |
| `hydroponics/cycle/reset` | to device | any value: Day 1 starts now |
| `hydroponics/cycle_status`, `pump_cycle_status` | from device | `phase,elapsed,total` (every second) |
| `hydroponics/tank/state` | from device | `full` / `empty` |
| `hydroponics/led1..4/enabled/set` / `state` | both | `1` in use / `0` not in use |
| `hydroponics/pump/override/set` / `state` | both | `on` / `off` / `none` |
| `hydroponics/ota/set` | to device | HTTPS URL of a `.bin` |
| `hydroponics/status` | from device | `online` / `offline` (last will) |
| `hydroponics/log` | from device | event text |
