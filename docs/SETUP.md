# Setup guide

How to deploy your own copy: backend (Worker + Durable Object), then website, then firmware.
No real passwords, keys or URLs are stored in this repository. Every value marked `YOUR-...`
is yours to fill in, and every secret lives in Cloudflare or in a git-ignored `secrets.h`.

## How the messaging works

The ESP32 and every open dashboard keep a secure WebSocket to the Worker at `/ws`. The Worker
passes each socket to one Durable Object, `HydroRelay`, which acts as a small broker:

| Role | How it is proven | Can receive | Can send |
| --- | --- | --- | --- |
| Device (ESP32) | `X-Device-Key` header = Worker secret `DEVICE_KEY` | Command topics it subscribes to | Any `hydroponics/...` topic |
| Viewer (public page) | Nothing | Any topic it subscribes to | Nothing |
| Admin (logged in) | Sends `{"t":"auth","token":...}`, checked against the D1 sessions table | Any topic it subscribes to | Only commands: `.../set`, `cycle/reset`, `ledN/planted` |

Frames are small JSON objects:

```
client -> relay   {"t":"sub","topic":"hydroponics/led1/state"}
                  {"t":"pub","topic":"hydroponics/led1/set","payload":"1","retain":false}
                  {"t":"auth","token":"<admin login token>"}
relay -> client   {"t":"msg","topic":"hydroponics/led1/state","payload":"1"}
                  {"t":"auth_ok"} | {"t":"auth_err"}
```

The relay keeps the latest value of each retained topic in its storage and sends it to anyone
who subscribes, like MQTT's retained messages. When the device's socket closes, it publishes
`hydroponics/status = offline` itself, which replaces MQTT's "last will".

## 1. Backend (Cloudflare Worker + Durable Object)

You need Node.js and a Cloudflare account. Run the commands inside `worker/`.

1. Log in: `npx wrangler login`
2. Create the storage and copy each printed ID into `wrangler.toml`:
   - `npx wrangler kv namespace create LOGS`, which gives the `id` under `[[kv_namespaces]]`
   - `npx wrangler d1 create hydro-db`, which gives the `database_id` under `[[d1_databases]]`
   (D1 tables are created automatically on first use.)
3. Set the secrets. Each command asks for the value, so it never lands in a file:
   - `npx wrangler secret put LOG_KEY`: a random string; the firmware's `LOG_API_KEY` must match
   - `npx wrangler secret put SETUP_KEY`: a long random string, needed to create admin accounts
   - `npx wrangler secret put DEVICE_KEY`: a long random string; the firmware's `DEVICE_KEY` must match
4. Deploy: `npx wrangler deploy`. The `[[migrations]]` block creates the `HydroRelay` Durable
   Object the first time. It is SQLite-backed (`new_sqlite_classes`), which the free plan supports.
5. Test: `<worker URL>/logs` returns `[]`, and `<worker URL>/auth/me` returns "Not logged in or session expired".

Make a random value with, for example, `openssl rand -hex 24`. Use a different one for each secret.

**Usage note:** the device publishes the two schedule progress topics once a second, which keeps
the Durable Object busy around the clock. Check its usage in the Cloudflare dashboard after the
first days. If it gets close to your plan's limits, raise that interval in `runSchedules()`.

## 2. Website

1. In `website/index.html` and `website/admin-setup.html`, set `API_BASE` to your Worker URL
   (no trailing slash). That is the only setting: the page holds no passwords.
2. Upload the whole `website` folder as one static-site deployment (for example Cloudflare Pages).
   Every upload replaces the whole site, so always include both files.
3. Open `admin-setup.html`, enter the `SETUP_KEY`, and create your first admin account.

## 3. Firmware (ESP32)

1. Copy `firmware/hydro_vOTA/secrets.example.h` to `secrets.h` in the same folder and fill it in:
   Wi-Fi, `RELAY_HOST` (the Worker's host name only, e.g. `hydro-api.YOUR-SUBDOMAIN.workers.dev`),
   `DEVICE_KEY`, `LOG_API_URL`, `LOG_API_KEY` and `OTA_PASSWORD`.
2. Arduino IDE settings:
   - Board package: **esp32** by Espressif
   - Libraries: **WebSockets** by Markus Sattler, **ArduinoJson** by Benoit Blanchon (v7)
   - Board: *ESP32-WROOM-DA Module* (or *ESP32 Dev Module*)
   - Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA)**, or any scheme "with OTA"
   - Serial Monitor: 115200 baud
3. Check the pin wiring in the `YOUR SETTINGS` block of `hydro_vOTA.ino`.
4. Upload over USB for the first install. After that, use either:
   - **Local network:** Tools → Port → `hydro-esp32 at 192.168.x.x` (asks for `OTA_PASSWORD`)
   - **Dashboard:** Sketch → Export Compiled Binary, drop the `.ino.bin` into the Admin tab, then press *Install*
5. Change `FW_VERSION` with every release so the dashboard shows which version is running.

The Serial Monitor shows `Relay connected` once the device reaches the Durable Object.

## Moving from the HiveMQ version

1. Deploy the backend (step 1) and the new website (step 2) first. The dashboard shows
   "System offline" until the device moves over.
2. Flash the new firmware **over USB or the local network** (Arduino IDE). The dashboard's update
   button can't reach the old firmware, because the new dashboard talks to the relay while the old
   firmware is still on HiveMQ.
3. Once the dashboard shows the device online, delete the HiveMQ cluster and its credentials.

## Topics (summary)

Same names as the MQTT version. "to device" = commands, "from device" = state.

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
