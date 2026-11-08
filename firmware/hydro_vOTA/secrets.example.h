// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore, so your real passwords are never committed.
#pragma once

// ---------------- WIFI ----------------
static const char* WIFI_SSID = "YOUR-WIFI-NAME";
static const char* WIFI_PASS = "YOUR-WIFI-PASSWORD";

// ---------------- MQTT BROKER (HiveMQ Cloud) ----------------
// Device credential: needs Publish + Subscribe permission on hydroponics/#
static const char* MQTT_HOST = "YOUR-CLUSTER.s1.eu.hivemq.cloud";
static const char* MQTT_USER = "YOUR-DEVICE-USER";
static const char* MQTT_PASS = "YOUR-DEVICE-PASSWORD";

// ---------------- EVENT LOG (Cloudflare Worker) ----------------
// While the URL still contains "YOUR-SUBDOMAIN", cloud logging is switched off automatically.
static const char* LOG_API_URL = "https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev/logs";
static const char* LOG_API_KEY = "SAME-VALUE-AS-THE-WORKER-LOG_KEY-SECRET";

// ---------------- WIRELESS UPDATES (local network) ----------------
static const char* OTA_PASSWORD = "CHOOSE-A-LONG-RANDOM-PASSWORD";
