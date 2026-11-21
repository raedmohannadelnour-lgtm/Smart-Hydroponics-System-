// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore, so your real passwords are never committed.
#pragma once

// ---------------- WIFI ----------------
static const char* WIFI_SSID = "YOUR-WIFI-NAME";
static const char* WIFI_PASS = "YOUR-WIFI-PASSWORD";

// ---------------- LIVE RELAY (Cloudflare Worker + Durable Object) ----------------
// Host name only: no "https://", no path. The firmware connects to wss://<host>/ws
static const char* RELAY_HOST = "YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev";
// Must match the Worker secret DEVICE_KEY. Use a long random string, used nowhere else.
static const char* DEVICE_KEY = "SAME-VALUE-AS-THE-WORKER-DEVICE_KEY-SECRET";

// ---------------- EVENT LOG (Cloudflare Worker) ----------------
// While the URL still contains "YOUR-SUBDOMAIN", cloud logging is switched off automatically.
static const char* LOG_API_URL = "https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev/logs";
static const char* LOG_API_KEY = "SAME-VALUE-AS-THE-WORKER-LOG_KEY-SECRET";

// ---------------- WIRELESS UPDATES (local network) ----------------
static const char* OTA_PASSWORD = "CHOOSE-A-LONG-RANDOM-PASSWORD";
