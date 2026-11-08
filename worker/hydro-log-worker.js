// Cloudflare Worker: Hydroponics backend
//   - Event log (KV)                       -> written by the ESP32, read by the dashboard
//   - Admin accounts, sessions, audit (D1) -> login, "who's online", "who did what"
//
// BINDINGS (Worker -> Settings -> Bindings):
//   LOGS  = KV namespace (already set up)
//   DB    = D1 database  (Storage & Databases -> D1 -> Create, e.g. "hydro-db")
//
// SECRETS (Worker -> Settings -> Variables and Secrets, type "Secret"):
//   LOG_KEY          already set; must match LOG_API_KEY in the firmware
//   SETUP_KEY        a long random string; needed only to create/reset admin accounts
//   MQTT_ADMIN_USER  HiveMQ credential with "Publish and Subscribe" permission
//   MQTT_ADMIN_PASS  its password
//   (given ONLY to logged-in admins, so it never appears in the public page)
//
// The D1 tables are created automatically on first use; no SQL console needed.
//
// ENDPOINTS
//   GET  /logs                 public: stored event log
//   POST /logs                 ESP32 only (X-Log-Key header)
//   POST /auth/login           {username, password} -> {token, user, mqtt}
//   GET  /auth/me              (Bearer) -> {user, mqtt}; also marks the session active
//   POST /auth/heartbeat       (Bearer) keeps "online now" accurate
//   POST /auth/logout          (Bearer)
//   POST /audit                (Bearer) {action}: records what an admin did
//   GET  /admin/overview       (Bearer) -> {online, logins, actions}
//   POST /auth/setup           {setupKey, username, displayName, password} create/reset account
//                              {setupKey, username, delete: true}          delete account
//
//   FIRMWARE (stored in the LOGS KV namespace, admin only; files contain passwords)
//   POST /firmware/upload?kind=bin|ino  (Bearer) raw file body; headers X-Filename, X-Version
//   GET  /firmware/info                 (Bearer) -> {bin, ino} details of the stored files
//   GET  /firmware/file?kind=bin|ino    (Bearer) download a stored file (backup)
//   POST /firmware/ota-link             (Bearer) -> {url}: private link for the ESP32, valid 15 min
//   GET  /firmware/ota/<token>.bin      (no login) the ESP32 downloads the firmware here

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization, X-Log-Key, X-Filename, X-Version",
  "Access-Control-Expose-Headers": "Content-Disposition",
};
const FW_MAX_BIN       = 4 * 1024 * 1024;   // app firmware is ~1-2 MB
const FW_MIN_BIN       = 100 * 1024;        // smaller = bootloader/partitions file, not the app
const FW_MAX_INO       = 1024 * 1024;
const OTA_LINK_TTL_SEC = 15 * 60;
const MAX_LOG_ENTRIES    = 200;
const MAX_MSG_LEN        = 200;
const SESSION_TTL_MS     = 7 * 24 * 60 * 60 * 1000;   // a login lasts 7 days
const ONLINE_WINDOW_MS   = 2 * 60 * 1000;             // "online" = heartbeat in the last 2 min
const FAIL_WINDOW_MS     = 15 * 60 * 1000;            // lockout window for wrong passwords
const MAX_FAILS_PER_USER = 5;
const MAX_FAILS_PER_IP   = 10;
const PBKDF2_ITERATIONS  = 100000;                    // Workers' maximum

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { ...CORS, "Content-Type": "application/json", "Cache-Control": "no-store" },
  });
const text = (body, status = 200) => new Response(body, { status, headers: CORS });

async function readJson(request) {
  try { return await request.json(); } catch { return null; }
}

// ---------------- Schema ----------------
let schemaReady = false;
async function ensureSchema(env) {
  if (schemaReady) return;
  await env.DB.batch([
    env.DB.prepare(`CREATE TABLE IF NOT EXISTS users (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      username TEXT UNIQUE NOT NULL,
      display_name TEXT NOT NULL,
      pw_hash TEXT NOT NULL,
      pw_salt TEXT NOT NULL,
      created_at INTEGER NOT NULL)`),
    env.DB.prepare(`CREATE TABLE IF NOT EXISTS sessions (
      token TEXT PRIMARY KEY,
      user_id INTEGER NOT NULL,
      created_at INTEGER NOT NULL,
      last_seen INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      ended_at INTEGER,
      ip TEXT,
      user_agent TEXT)`),
    env.DB.prepare(`CREATE TABLE IF NOT EXISTS actions (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      user_id INTEGER NOT NULL,
      ts INTEGER NOT NULL,
      action TEXT NOT NULL)`),
    env.DB.prepare(`CREATE TABLE IF NOT EXISTS login_failures (
      username TEXT, ip TEXT, ts INTEGER NOT NULL)`),
    env.DB.prepare(`CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id)`),
    env.DB.prepare(`CREATE INDEX IF NOT EXISTS idx_actions_ts ON actions(ts)`),
  ]);
  schemaReady = true;
}

// ---------------- Crypto helpers ----------------
const enc = new TextEncoder();
const toHex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
const fromHex = (hex) => Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16)));
function randomHex(bytes) {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return toHex(a);
}
async function hashPassword(password, saltHex) {
  const key = await crypto.subtle.importKey("raw", enc.encode(password), "PBKDF2", false, ["deriveBits"]);
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", hash: "SHA-256", salt: fromHex(saltHex), iterations: PBKDF2_ITERATIONS },
    key, 256);
  return toHex(bits);
}
function safeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let r = 0;
  for (let i = 0; i < a.length; i++) r |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return r === 0;
}

const publicUser = (u) => ({ username: u.username, displayName: u.display_name });
const mqttCreds = (env) =>
  env.MQTT_ADMIN_USER && env.MQTT_ADMIN_PASS
    ? { username: env.MQTT_ADMIN_USER, password: env.MQTT_ADMIN_PASS }
    : null;

async function getSession(request, env) {
  const m = (request.headers.get("Authorization") || "").match(/^Bearer ([a-f0-9]{64})$/);
  if (!m) return null;
  const row = await env.DB.prepare(
    `SELECT s.token, s.user_id, s.expires_at, s.ended_at, u.username, u.display_name
       FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token = ?`
  ).bind(m[1]).first();
  if (!row || row.ended_at || row.expires_at < Date.now()) return null;
  return row;
}
async function touch(env, token) {
  await env.DB.prepare("UPDATE sessions SET last_seen = ? WHERE token = ?").bind(Date.now(), token).run();
}

// ---------------- Handlers ----------------
async function handleLogin(request, env) {
  const body = await readJson(request);
  const username = String(body?.username || "").trim().toLowerCase();
  const password = String(body?.password || "");
  if (!username || !password) return json({ error: "Enter a username and password" }, 400);

  const ip = request.headers.get("CF-Connecting-IP") || "";
  const now = Date.now();

  const fails = await env.DB.prepare(
    `SELECT SUM(username = ?) AS byUser, SUM(ip = ?) AS byIp FROM login_failures WHERE ts > ?`
  ).bind(username, ip, now - FAIL_WINDOW_MS).first();
  if ((fails?.byUser || 0) >= MAX_FAILS_PER_USER || (fails?.byIp || 0) >= MAX_FAILS_PER_IP) {
    return json({ error: "Too many wrong attempts. Try again in 15 minutes." }, 429);
  }

  const user = await env.DB.prepare("SELECT * FROM users WHERE username = ?").bind(username).first();
  const ok = user && safeEqual(await hashPassword(password, user.pw_salt), user.pw_hash);
  if (!ok) {
    await env.DB.batch([
      env.DB.prepare("INSERT INTO login_failures (username, ip, ts) VALUES (?, ?, ?)").bind(username, ip, now),
      env.DB.prepare("DELETE FROM login_failures WHERE ts < ?").bind(now - 24 * 60 * 60 * 1000),
    ]);
    return json({ error: "Wrong username or password" }, 401);
  }

  const creds = mqttCreds(env);
  if (!creds) return json({ error: "Server missing MQTT_ADMIN_USER / MQTT_ADMIN_PASS secrets" }, 500);

  const token = randomHex(32);
  await env.DB.prepare(
    `INSERT INTO sessions (token, user_id, created_at, last_seen, expires_at, ip, user_agent)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).bind(token, user.id, now, now, now + SESSION_TTL_MS, ip,
         (request.headers.get("User-Agent") || "").slice(0, 200)).run();

  return json({ token, user: publicUser(user), mqtt: creds });
}

async function handleSetup(request, env) {
  if (!env.SETUP_KEY) return json({ error: "SETUP_KEY secret is not configured on the Worker" }, 500);
  const body = await readJson(request);
  if (!body || !safeEqual(String(body.setupKey || ""), env.SETUP_KEY)) {
    return json({ error: "Wrong setup key" }, 401);
  }
  const username = String(body.username || "").trim().toLowerCase();
  if (!/^[a-z0-9_.-]{3,32}$/.test(username)) {
    return json({ error: "Username: 3-32 characters, letters/numbers/._- only" }, 400);
  }

  if (body.delete) {
    const u = await env.DB.prepare("SELECT id FROM users WHERE username = ?").bind(username).first();
    if (!u) return json({ error: "No such account" }, 404);
    await env.DB.batch([
      env.DB.prepare("UPDATE sessions SET ended_at = ? WHERE user_id = ? AND ended_at IS NULL").bind(Date.now(), u.id),
      env.DB.prepare("DELETE FROM users WHERE id = ?").bind(u.id),
    ]);
    return json({ ok: true, message: `Deleted account "${username}"` });
  }

  const displayName = String(body.displayName || "").trim().slice(0, 40) || username;
  const password = String(body.password || "");
  if (password.length < 8) return json({ error: "Password must be at least 8 characters" }, 400);

  const existing = await env.DB.prepare("SELECT id FROM users WHERE username = ?").bind(username).first();
  const salt = randomHex(16);
  const hash = await hashPassword(password, salt);
  const now = Date.now();

  if (existing) {
    // Password reset: also signs that admin out everywhere
    await env.DB.batch([
      env.DB.prepare("UPDATE users SET display_name = ?, pw_hash = ?, pw_salt = ? WHERE id = ?")
        .bind(displayName, hash, salt, existing.id),
      env.DB.prepare("UPDATE sessions SET ended_at = ? WHERE user_id = ? AND ended_at IS NULL")
        .bind(now, existing.id),
    ]);
    return json({ ok: true, message: `Updated account "${username}" (signed out of all devices)` });
  }
  await env.DB.prepare(
    "INSERT INTO users (username, display_name, pw_hash, pw_salt, created_at) VALUES (?, ?, ?, ?, ?)"
  ).bind(username, displayName, hash, salt, now).run();
  return json({ ok: true, message: `Created account "${username}"` });
}

async function handleOverview(env) {
  const now = Date.now();
  const [online, logins, actions] = await env.DB.batch([
    env.DB.prepare(
      `SELECT u.display_name AS name, s.created_at, s.last_seen, s.user_agent
         FROM sessions s JOIN users u ON u.id = s.user_id
        WHERE s.ended_at IS NULL AND s.expires_at > ? AND s.last_seen > ?
        ORDER BY s.last_seen DESC`
    ).bind(now, now - ONLINE_WINDOW_MS),
    env.DB.prepare(
      `SELECT u.display_name AS name, s.created_at, s.last_seen, s.ended_at, s.expires_at, s.ip, s.user_agent
         FROM sessions s JOIN users u ON u.id = s.user_id
        ORDER BY s.created_at DESC LIMIT 50`
    ),
    env.DB.prepare(
      `SELECT u.display_name AS name, a.ts, a.action
         FROM actions a JOIN users u ON u.id = a.user_id
        ORDER BY a.ts DESC LIMIT 100`
    ),
  ]);
  return json({ now, onlineWindowMs: ONLINE_WINDOW_MS,
                online: online.results, logins: logins.results, actions: actions.results });
}

// ---------------- Log (KV), unchanged behaviour ----------------
async function handleLogPost(request, env) {
  if (!env.LOG_KEY) return text("LOG_KEY secret not configured", 500);
  if (request.headers.get("X-Log-Key") !== env.LOG_KEY) return text("Unauthorized", 401);
  const entry = await readJson(request);
  if (!entry || typeof entry.msg !== "string" || !entry.msg.trim()) return text("Expected {msg}", 400);

  const raw = await env.LOGS.get("log");
  const list = raw ? JSON.parse(raw) : [];
  list.unshift({ ts: Date.now(), msg: entry.msg.trim().slice(0, MAX_MSG_LEN) });
  if (list.length > MAX_LOG_ENTRIES) list.length = MAX_LOG_ENTRIES;
  await env.LOGS.put("log", JSON.stringify(list));
  return text("ok");
}

// ---------------- Firmware storage ----------------
async function handleFirmwareUpload(request, env, session, kind) {
  if (kind !== "bin" && kind !== "ino") return json({ error: "kind must be bin or ino" }, 400);
  const buf = await request.arrayBuffer();
  const size = buf.byteLength;
  if (!size) return json({ error: "Empty file" }, 400);
  if (kind === "bin") {
    if (size > FW_MAX_BIN) return json({ error: "File too large for ESP32 firmware" }, 400);
    if (new Uint8Array(buf)[0] !== 0xE9 || size < FW_MIN_BIN) {
      return json({ error: "Not the app firmware. Use the .ino.bin from Sketch -> Export Compiled Binary (not bootloader, partitions or merged)." }, 400);
    }
  } else if (size > FW_MAX_INO) {
    return json({ error: "Source file too large" }, 400);
  }
  const dec = (h) => { try { return decodeURIComponent(h || ""); } catch { return h || ""; } };
  const name = dec(request.headers.get("X-Filename")).slice(0, 100) || `hydro.${kind}`;
  const version = dec(request.headers.get("X-Version")).slice(0, 40);
  const meta = { name, size, version, at: Date.now(), by: session.display_name };
  await env.LOGS.put(`fwfile:${kind}`, buf, { metadata: meta });
  await env.DB.prepare("INSERT INTO actions (user_id, ts, action) VALUES (?, ?, ?)")
    .bind(session.user_id, Date.now(), `Uploaded ${name} (${(size / 1048576).toFixed(2)} MB)${version ? " v" + version : ""}`).run();
  return json({ ok: true, ...meta });
}

async function handleFirmwareInfo(env) {
  const out = { bin: null, ino: null };
  const list = await env.LOGS.list({ prefix: "fwfile:" });
  for (const k of list.keys) out[k.name.slice(7)] = k.metadata || null;
  return json(out);
}

async function handleFirmwareFile(env, kind) {
  if (kind !== "bin" && kind !== "ino") return json({ error: "kind must be bin or ino" }, 400);
  const { value, metadata } = await env.LOGS.getWithMetadata(`fwfile:${kind}`, { type: "arrayBuffer" });
  if (!value) return json({ error: "No file stored yet" }, 404);
  const name = (metadata && metadata.name) || `hydro.${kind}`;
  return new Response(value, { headers: { ...CORS,
    "Content-Type": kind === "ino" ? "text/plain; charset=utf-8" : "application/octet-stream",
    "Content-Disposition": `attachment; filename="${name.replace(/[^\w.\-]/g, "_")}"`,
    "Cache-Control": "no-store" } });
}

async function handleOtaLink(request, env) {
  const { metadata } = await env.LOGS.getWithMetadata("fwfile:bin");
  if (!metadata) return json({ error: "No firmware uploaded yet" }, 404);
  const token = randomHex(32);
  await env.LOGS.put(`fwtok:${token}`, "1", { expirationTtl: OTA_LINK_TTL_SEC });
  const url = `${new URL(request.url).origin}/firmware/ota/${token}.bin`;
  return json({ url, expiresInSec: OTA_LINK_TTL_SEC, name: metadata.name, version: metadata.version || "" });
}

// The ESP32 has no admin login, so it gets a random link that expires after 15 minutes
async function handleOtaDownload(env, token) {
  if (!(await env.LOGS.get(`fwtok:${token}`))) return text("Link expired or invalid", 403);
  const value = await env.LOGS.get("fwfile:bin", { type: "arrayBuffer" });
  if (!value) return text("No firmware stored", 404);
  return new Response(value, { headers: { "Content-Type": "application/octet-stream", "Cache-Control": "no-store" } });
}

// ---------------- Router ----------------
export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    const method = request.method;
    if (method === "OPTIONS") return new Response(null, { headers: CORS });

    try {
      if (pathname === "/logs" && method === "GET") {
        const raw = await env.LOGS.get("log");
        return new Response(raw || "[]", {
          headers: { ...CORS, "Content-Type": "application/json", "Cache-Control": "no-store" },
        });
      }
      if (pathname === "/logs" && method === "POST") return handleLogPost(request, env);

      const ota = pathname.match(/^\/firmware\/ota\/([a-f0-9]{64})\.bin$/);
      if (ota && method === "GET") return handleOtaDownload(env, ota[1]);

      if (!pathname.startsWith("/auth/") && !pathname.startsWith("/admin/") &&
          !pathname.startsWith("/firmware/") && pathname !== "/audit") {
        return text("Not found", 404);
      }
      if (!env.DB) return json({ error: "D1 binding 'DB' is not configured on the Worker" }, 500);
      await ensureSchema(env);

      if (pathname === "/auth/login" && method === "POST") return handleLogin(request, env);
      if (pathname === "/auth/setup" && method === "POST") return handleSetup(request, env);

      // Everything below requires a valid admin session
      const session = await getSession(request, env);
      if (!session) return json({ error: "Not logged in or session expired" }, 401);

      if (pathname === "/auth/me" && method === "GET") {
        await touch(env, session.token);
        return json({ user: publicUser(session), mqtt: mqttCreds(env) });
      }
      if (pathname === "/auth/heartbeat" && method === "POST") {
        await touch(env, session.token);
        return json({ ok: true });
      }
      if (pathname === "/auth/logout" && method === "POST") {
        await env.DB.prepare("UPDATE sessions SET ended_at = ? WHERE token = ?").bind(Date.now(), session.token).run();
        return json({ ok: true });
      }
      if (pathname === "/audit" && method === "POST") {
        const body = await readJson(request);
        const action = String(body?.action || "").trim().slice(0, 120);
        if (!action) return json({ error: "Expected {action}" }, 400);
        const now = Date.now();
        await env.DB.batch([
          env.DB.prepare("INSERT INTO actions (user_id, ts, action) VALUES (?, ?, ?)").bind(session.user_id, now, action),
          env.DB.prepare("UPDATE sessions SET last_seen = ? WHERE token = ?").bind(now, session.token),
        ]);
        return json({ ok: true });
      }
      if (pathname === "/admin/overview" && method === "GET") {
        await touch(env, session.token);
        return handleOverview(env);
      }
      const kind = new URL(request.url).searchParams.get("kind");
      if (pathname === "/firmware/upload" && method === "POST") return handleFirmwareUpload(request, env, session, kind);
      if (pathname === "/firmware/info" && method === "GET") return handleFirmwareInfo(env);
      if (pathname === "/firmware/file" && method === "GET") return handleFirmwareFile(env, kind);
      if (pathname === "/firmware/ota-link" && method === "POST") return handleOtaLink(request, env);
      return text("Not found", 404);
    } catch (err) {
      return json({ error: "Server error: " + (err && err.message ? err.message : String(err)) }, 500);
    }
  },
};
