// ESPBlox - Run Vibeblox on ESP32!
// ---------------------------------------------------------------
// Converted from the zero-dependency Node.js server. Every player generates
// the world from the same seed, so the ESP32 only has to share what CHANGES:
// block edits, player positions, chat, and the time of day. Block edits are
// saved to world.json on the LittleFS partition so the world survives restarts.
//
// Runtime server settings live in LittleFS:/settings.json, even when SD is
// selected for world/player data. UART: `config` shows values and
// `config set <key> <value>` updates a value and reboots to apply it.
// Libraries: WiFi, LittleFS, HTTPClient, mbedtls (all built into the ESP32
// Arduino core) + ArduinoJson (install "bblanchon/ArduinoJson" 6.x).
// Partition scheme: pick one with a LittleFS/spiffs partition.

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <limits.h>
#include <stdlib.h>
#include <new>
// ---------------- Build-time protocol and storage settings ----------------
#define WORLD_FILE          "/world.json"
#define SAFE_WORLD_FILE     "/world.safe.json"
#define WORLD_LOAD_MARKER   "/world.load.inprogress"
#define SAFE_MODE_MARKER    "/world.safe-mode"


#define PROTOCOL            17   // must match PROTOCOL_VERSION in the game
#define DAY_LENGTH          600  // seconds, must match the game
#define MAX_FRAME           32768 // accepts updated Node server's combined custom look payloads
#define SLEEP_SECONDS       3    // must match the game
#define MAX_CHANGES         4096  // base cached block-edit capacity without PSRAM
#define WORLD_CHANGE_FLUSH_SECONDS 60UL // seconds between saving and clearing the RAM change cache
#define JOIN_BLOCK_BATCH    128   // keep initial world-sync WebSocket messages small
#define MAX_BANS            100
#define MAX_SIGNS           128
#define MAX_WS_PLAYERS      24   // base player slots without PSRAM
#define MAX_MUTES           32
#define PLAYER_IDLE_TIMEOUT_MS 0UL // disabled; WebSocket ping/pong keeps idle connections alive
#define MAX_SAVED_PLAYERS 1024
#define PLAYER_DATA_FILE "/players.json"
#define FILE_MANAGER_ENABLED 1
#define FILE_MANAGER_MAX_UPLOAD (512UL * 1024UL)
#define UART_DEBUG          0
#define PLAYER_POS_DEBUG    1
struct ServerConfig {
  uint16_t port;
  int maxPlayers;
  long seed;
  char worldType[12];
  char defaultMode[12];
  bool weatherCycle;
  bool experimental;
  char wifiSsid[33];
  char wifiPassword[65];
  char supabaseUrl[192];
  char supabaseAnonKey[128];
  char adminPassword[64];
};
static ServerConfig serverConfig = {
  27631, 0, 0, "default", "creative", true, false,
  "SSID", "PASSWORD",
  "https://okhtosecqytklgwouwxv.supabase.co",
  "sb_publishable_kyklbTdQk64bRSdGSrEX0w_1lKAD4X0", "changeme"
};
static bool littleFsReady = false;
static const char* SETTINGS_FILE = "/settings.json";
static int maxWsPlayers = MAX_WS_PLAYERS;
static int maxChangeCapacity = MAX_CHANGES;
#if UART_DEBUG
#define DBG(...) do { Serial.printf("[DBG %lu] ", (unsigned long)millis()); Serial.printf(__VA_ARGS__); Serial.print("\r\n"); } while (0)
#else
#define DBG(...) do {} while (0)
#endif

// ---------------- Small helpers ----------------
static String jsonEsc(const String& s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((unsigned char)c < 0x20) {
      char b[8];
      snprintf(b, 8, "\\u%04x", c);
      out += b;
    }
    else out += c;
  }
  return out + "\"";
}

static void copyConfigText(char* dst, size_t dstSize, const char* src) {
  if (!dst || !dstSize || !src) return;
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = 0;
}

static long makeRandomWorldSeed() {
  uint32_t seed = esp_random() & 0x7FFFFFFFUL;
  return seed ? (long)seed : 1L;
}

static bool saveServerConfig() {
  if (!littleFsReady) {
    Serial.println("[CONFIG] ERROR: LittleFS is unavailable; settings were not saved");
    return false;
  }
  DynamicJsonDocument doc(2048);
  doc["port"] = serverConfig.port;
  doc["max_players"] = serverConfig.maxPlayers;
  doc["seed"] = serverConfig.seed;
  doc["world_type"] = serverConfig.worldType;
  doc["default_mode"] = serverConfig.defaultMode;
  doc["weather_cycle"] = serverConfig.weatherCycle;
  doc["experimental"] = serverConfig.experimental;
  doc["wifi_ssid"] = serverConfig.wifiSsid;
  doc["wifi_password"] = serverConfig.wifiPassword;
  doc["supabase_url"] = serverConfig.supabaseUrl;
  doc["supabase_anon_key"] = serverConfig.supabaseAnonKey;
  doc["admin_password"] = serverConfig.adminPassword;

  String tmp = String(SETTINGS_FILE) + ".tmp";
  String backup = String(SETTINGS_FILE) + ".bak";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.println("[CONFIG] ERROR: cannot open settings.json.tmp in LittleFS");
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.flush();
  bool writeError = f.getWriteError() != 0;
  size_t fileSize = f.size();
  f.close();
  if (writeError || !written || fileSize != written) {
    LittleFS.remove(tmp);
    Serial.printf("[CONFIG] ERROR: settings write incomplete (%u/%u bytes)\r\n", (unsigned)fileSize, (unsigned)written);
    return false;
  }

  LittleFS.remove(backup);
  bool hadOld = LittleFS.exists(SETTINGS_FILE);
  if (hadOld && !LittleFS.rename(SETTINGS_FILE, backup)) {
    LittleFS.remove(tmp);
    Serial.println("[CONFIG] ERROR: could not preserve existing settings.json");
    return false;
  }
  if (!LittleFS.rename(tmp, SETTINGS_FILE)) {
    if (hadOld) LittleFS.rename(backup, SETTINGS_FILE);
    LittleFS.remove(tmp);
    Serial.println("[CONFIG] ERROR: could not install settings.json; old settings restored");
    return false;
  }
  if (hadOld) LittleFS.remove(backup);
  Serial.println("[CONFIG] Saved settings.json in LittleFS");
  return true;
}

static void loadServerConfig() {
  if (!littleFsReady) {
    Serial.println("[CONFIG] LittleFS unavailable; using built-in defaults");
    return;
  }
  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) {
    serverConfig.seed = makeRandomWorldSeed();
    Serial.printf("[CONFIG] settings.json not found; generated random default seed=%ld\r\n", serverConfig.seed);
    saveServerConfig();
    return;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, f);
  f.close();
  if (error || !doc.is<JsonObject>()) {
    Serial.printf("[CONFIG] Invalid settings.json (%s); using defaults\r\n", error ? error.c_str() : "root is not an object");
    return;
  }
  int port = doc["port"] | (int)serverConfig.port;
  int maxPlayers = doc["max_players"] | serverConfig.maxPlayers;
  long seed = doc["seed"] | serverConfig.seed;
  if (port >= 1 && port <= 65535) serverConfig.port = (uint16_t)port;
  if (maxPlayers >= 0 && maxPlayers <= maxWsPlayers) serverConfig.maxPlayers = maxPlayers;
  bool generatedSeed = seed <= 0;
  serverConfig.seed = generatedSeed ? makeRandomWorldSeed() : seed;
  const char* worldType = doc["world_type"] | serverConfig.worldType;
  if (!strcmp(worldType, "default") || !strcmp(worldType, "superflat")) copyConfigText(serverConfig.worldType, sizeof(serverConfig.worldType), worldType);
  const char* defaultMode = doc["default_mode"] | serverConfig.defaultMode;
  bool validMode = !strcmp(defaultMode, "survival") || !strcmp(defaultMode, "creative") || !strcmp(defaultMode, "adventure");
  if (validMode) copyConfigText(serverConfig.defaultMode, sizeof(serverConfig.defaultMode), defaultMode);
  serverConfig.weatherCycle = doc["weather_cycle"] | serverConfig.weatherCycle;
  serverConfig.experimental = doc["experimental"] | serverConfig.experimental;
  copyConfigText(serverConfig.wifiSsid, sizeof(serverConfig.wifiSsid), doc["wifi_ssid"] | serverConfig.wifiSsid);
  copyConfigText(serverConfig.wifiPassword, sizeof(serverConfig.wifiPassword), doc["wifi_password"] | serverConfig.wifiPassword);
  copyConfigText(serverConfig.supabaseUrl, sizeof(serverConfig.supabaseUrl), doc["supabase_url"] | serverConfig.supabaseUrl);
  copyConfigText(serverConfig.supabaseAnonKey, sizeof(serverConfig.supabaseAnonKey), doc["supabase_anon_key"] | serverConfig.supabaseAnonKey);
  copyConfigText(serverConfig.adminPassword, sizeof(serverConfig.adminPassword), doc["admin_password"] | serverConfig.adminPassword);
  if (generatedSeed) {
    Serial.printf("[CONFIG] Missing or invalid seed; generated random seed=%ld\r\n", serverConfig.seed);
    saveServerConfig();
  }
  Serial.printf("[CONFIG] Loaded settings.json from LittleFS (port=%u, max_players=%d)\r\n", serverConfig.port, serverConfig.maxPlayers);
}
static bool isNum(JsonVariant v, double* out = nullptr) {
  if (!v.is<double>() && !v.is<int>() && !v.is<long>()) return false;
  double d = v.as<double>();
  if (isnan(d) || isinf(d)) return false;
  if (out) *out = d;
  return true;
}

// ---------------- World state ----------------
static const char* DIFFICULTIES[] = {"peaceful", "easy", "normal", "hard"};
static const char* WEATHERS[] = {"clear", "rain", "snow", "thunder"};
static const char* VALID_MODES[] = {"survival", "creative", "adventure"};
static const char* OWNER_NAME = "master";

// Host creator tier caps (kept in step with PLAYER_CAP in src/ranks.ts)
static const int CAPS[4] = {18, 24, 32, 48}; // none, rising, popular, star
static const long TIER_PLAYS[3] = {1000, 10000, 100000}; // rising, popular, star
enum Tier { T_NONE = 0, T_RISING = 1, T_POPULAR = 2, T_STAR = 3 };
static Tier hostTier = T_NONE;
static int maxPlayers() {
  int requested = serverConfig.maxPlayers > 0 ? serverConfig.maxPlayers : CAPS[hostTier];
  return requested < maxWsPlayers ? requested : maxWsPlayers;
}
static Tier tierOfPlays(long plays) {
  if (plays >= TIER_PLAYS[2]) return T_STAR;
  if (plays >= TIER_PLAYS[1]) return T_POPULAR;
  if (plays >= TIER_PLAYS[0]) return T_RISING;
  return T_NONE;
}
static const char* tierName(Tier t) {
  switch (t) {
    case T_STAR: return "star"; case T_POPULAR: return "popular";
  case T_RISING: return "rising"; default: return "none";
  }
}

struct Settings {
  char difficulty[10];
  bool keepInventory, scripts, mods, commands, alwaysDay, weatherCycle;
  char scriptPerm[8]; // off | all | admin | owner
};
static Settings settings = {"normal", false, true, true, true, false, true, "all"};

struct RankDef {
  char key[17];
  char name[17];
  char tag[81];
};
struct RankMember {
  char player[17];
  char rank[17];
};
struct Ranks {
  RankDef defs[30]; int nDefs;
  RankMember members[200]; int nMembers;
};
static Ranks ranks = {};

struct World {
  long seed;
  char worldType[12];
  char mode[12];
  struct Change {
    int32_t x, y, z, b;
  } *changes;
  int nChanges;
  struct Sign {
    int32_t x, y, z;
    char lines[4][16];
  } signs[MAX_SIGNS];
  int nSigns;
  double time;
  char bans[MAX_BANS][17];
  int nBans;
  char weather[8];
  bool experimental;
};
static World world = {};
static int changeCapacity = 0;
static bool worldChangesInPsram = false;
struct SavedPlayerData {
  char name[17];
  float x, y, z, yaw, pitch;
  char mode[13];
  int held;
  uint32_t updated;
};
static SavedPlayerData savedPlayers[MAX_SAVED_PLAYERS] = {};
static int nSavedPlayers = 0;
static char serverOwner[17] = "";
static bool playerDataDirty = false;
static bool worldDirty = true;
static void saveWorld();
static uint32_t lastChangeCacheFlush = 0;
// All persistent server data goes to the selected filesystem. SD.begin() uses
// the board's default SPI bus and SS/CS pin (the first/default SPI slot).
static fs::FS* storageFS = nullptr;
static bool storageIsSD = false;
static bool safeMode = false;
static String activeWorldFile = WORLD_FILE;
static double timeBaseValue = 0.02;
static uint32_t timeBaseAt = 0;

// ArduinoJson can parse a Stream directly. Buffer File reads in 1 KB blocks
// so flash access stays bounded and does not allocate a second world-sized RAM copy.
class ChunkedFileStream : public Stream {
 public:
  explicit ChunkedFileStream(File& file) : file_(file), pos_(0), len_(0) {}
  int available() override {
    size_t pending = len_ - pos_;
    size_t later = file_.available();
    size_t total = pending + later;
    return total > (size_t)INT_MAX ? INT_MAX : (int)total;
  }
  int read() override {
    if (!refill()) return -1;
    return buffer_[pos_++];
  }
  int peek() override {
    if (!refill()) return -1;
    return buffer_[pos_];
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }

 private:
  bool refill() {
    if (pos_ < len_) return true;
    len_ = file_.read(buffer_, sizeof(buffer_));
    pos_ = 0;
    return len_ > 0;
  }
  File& file_;
  uint8_t buffer_[1024];
  size_t pos_;
  size_t len_;
};

static bool isRankNameOk(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 16) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!isalnum((unsigned char)c) && c != '_') return false;
  }
  return true;
}
static bool isPlayerNameOk(const char* s) {
  size_t n = strlen(s);
  if (n < 3 || n > 16) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!isalnum((unsigned char)c) && c != '_') return false;
  }
  return true;
}

static bool setWorldSign(int32_t x, int32_t y, int32_t z, const char lines[4][16]) {
  int idx = -1;
  for (int i = 0; i < world.nSigns; i++)
    if (world.signs[i].x == x && world.signs[i].y == y && world.signs[i].z == z) {
      idx = i;
      break;
    }
  bool empty = true;
  for (int i = 0; i < 4; i++) if (lines[i][0]) {
      empty = false;
      break;
    }
  if (empty) {
    if (idx >= 0) {
      memmove(&world.signs[idx], &world.signs[idx + 1], sizeof(world.signs[0]) * (world.nSigns - idx - 1));
      world.nSigns--;
    }
    return true;
  }
  if (idx < 0) {
    if (world.nSigns >= MAX_SIGNS) return false;
    idx = world.nSigns++;
  }
  World::Sign& sign = world.signs[idx];
  sign.x = x; sign.y = y; sign.z = z;
  memcpy(sign.lines, lines, sizeof(sign.lines));
  return true;
}

// Same rules as cleanFakeRanks in src/fakeranks.ts
static void loadPlayerData() {
  nSavedPlayers = 0;
  File f = storageFS->open(PLAYER_DATA_FILE, "r");
  if (!f) return;
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err || !doc["players"].is<JsonArray>()) {
    DBG("Player data file invalid; starting empty");
    return;
  }
  JsonArray rows = doc["players"].as<JsonArray>();
  for (JsonVariant v : rows) {
    if (nSavedPlayers >= MAX_SAVED_PLAYERS || !v.is<JsonObject>()) break;
    JsonObject row = v.as<JsonObject>();
    const char* name = row["name"] | "";
    double x, y, z, yaw, pitch;
    if (!isPlayerNameOk(name) || !isNum(row["x"], &x) || !isNum(row["y"], &y) ||
        !isNum(row["z"], &z) || !isNum(row["yaw"], &yaw) || !isNum(row["pitch"], &pitch)) continue;
    SavedPlayerData& d = savedPlayers[nSavedPlayers];
    strncpy(d.name, name, 16); d.name[16] = 0;
    for (char* c = d.name; *c; c++) *c = tolower((unsigned char) * c);
    d.x = x; d.y = y; d.z = z; d.yaw = yaw; d.pitch = pitch;
    const char* mode = row["mode"] | "survival";
    bool validMode = false;
    for (auto m : VALID_MODES) if (!strcmp(mode, m)) validMode = true;
    strncpy(d.mode, validMode ? mode : "survival", 12); d.mode[12] = 0;
    int held = row["held"] | 0;
    d.held = held >= 0 && held < 4000 ? held : 0;
    d.updated = nSavedPlayers;
    nSavedPlayers++;
  }
  DBG("Loaded %d saved player records", nSavedPlayers);
}

static bool savePlayerData() {
  if (!playerDataDirty) return true;
  String tmp = String(PLAYER_DATA_FILE) + ".tmp";
  File f = storageFS->open(tmp, "w");
  if (!f) {
    DBG("ERROR: cannot open player save file %s", tmp.c_str());
    return false;
  }
  f.print("{\"players\":[");
  for (int i = 0; i < nSavedPlayers; i++) {
    if (i) f.print(",");
    const SavedPlayerData& d = savedPlayers[i];
    f.print("{\"name\":"); f.print(jsonEsc(d.name));
    f.printf(",\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"yaw\":%.4f,\"pitch\":%.4f,\"mode\":", d.x, d.y, d.z, d.yaw, d.pitch);
    f.print(jsonEsc(d.mode));
    f.printf(",\"held\":%d}", d.held);
  }
  f.print("]}");
  f.close();
  storageFS->remove(PLAYER_DATA_FILE);
  if (!storageFS->rename(tmp, PLAYER_DATA_FILE)) {
    DBG("ERROR: cannot replace player save file");
    return false;
  }
  playerDataDirty = false;
  DBG("Saved %d player records", nSavedPlayers);
  return true;
}
static void cleanRanks(JsonVariant v) {
  ranks.nDefs = 0; ranks.nMembers = 0;
  if (v.isNull() || !v.is<JsonObject>()) return;
  JsonObject vo = v.as<JsonObject>();
  JsonObject defs = vo["defs"];
  if (!defs.isNull()) {
    for (JsonPair kv : defs) {
      if (ranks.nDefs >= 30) break;
      const char* k = kv.key().c_str();
      JsonObject d = kv.value();
      if (d.isNull()) continue;
      const char* name = d["name"] | (const char*)nullptr;
      const char* tag = d["tag"] | (const char*)nullptr;
      if (!name || !tag || !isRankNameOk(name)) continue;
      char lk[17]; strncpy(lk, k, 16); lk[16] = 0;
      for (char* c = lk; *c; c++) *c = tolower((unsigned char) * c);
      if (strcmp(lk, name) != 0) continue; // key must equal name lowercased
      RankDef& rd = ranks.defs[ranks.nDefs++];
      strncpy(rd.key, lk, 16); rd.key[16] = 0;
      strncpy(rd.name, name, 16); rd.name[16] = 0;
      strncpy(rd.tag, tag, 80); rd.tag[80] = 0;
    }
  }
  JsonObject members = vo["members"];
  if (!members.isNull()) {
    for (JsonPair kv : members) {
      if (ranks.nMembers >= 200) break;
      const char* pl = kv.key().c_str();
      const char* r = kv.value() | (const char*)nullptr;
      if (!r || !isPlayerNameOk(pl)) continue;
      bool hasDef = false;
      for (int i = 0; i < ranks.nDefs; i++) if (strcmp(ranks.defs[i].key, r) == 0) {
          hasDef = true;
          break;
        }
      if (!hasDef) continue;
      bool lower = true;
      for (const char* c = pl; *c; c++) if (isupper((unsigned char)*c)) {
          lower = false;
          break;
        }
      if (!lower) continue;
      RankMember& rm = ranks.members[ranks.nMembers];
      ranks.nMembers++;
      strncpy(rm.player, pl, 16); rm.player[16] = 0;
      strncpy(rm.rank, r, 16); rm.rank[16] = 0;
    }
  }
}

static String ranksJson() {
  String s = "{\"defs\":{";
  for (int i = 0; i < ranks.nDefs; i++) {
    if (i) s += ",";
    s += String(jsonEsc(ranks.defs[i].key)) + ":{\"name\":" + jsonEsc(ranks.defs[i].name) + ",\"tag\":" + jsonEsc(ranks.defs[i].tag) + "}";
  }
  s += "},\"members\":{";
  for (int i = 0; i < ranks.nMembers; i++) {
    if (i) s += ",";
    s += String(jsonEsc(ranks.members[i].player)) + ":" + jsonEsc(ranks.members[i].rank);
  }
  s += "}}";
  return s;
}

static void applySettingsJson(JsonObject s) {
  const char* diff = s["difficulty"] | (const char*)nullptr;
  if (diff) for (auto d : DIFFICULTIES) if (strcmp(diff, d) == 0) {
        strncpy(settings.difficulty, diff, 9);
        settings.difficulty[9] = 0;
      }
  if (s["keepInventory"].is<bool>()) settings.keepInventory = s["keepInventory"];
  if (s["scripts"].is<bool>()) settings.scripts = s["scripts"];
  if (s["mods"].is<bool>()) settings.mods = s["mods"];
  if (s["commands"].is<bool>()) settings.commands = s["commands"];
  if (s["alwaysDay"].is<bool>()) settings.alwaysDay = s["alwaysDay"];
  if (s["weatherCycle"].is<bool>()) settings.weatherCycle = s["weatherCycle"];
  const char* sp = s["scriptPerm"] | (const char*)nullptr;
  if (sp && (!strcmp(sp, "off") || !strcmp(sp, "all") || !strcmp(sp, "admin") || !strcmp(sp, "owner"))) {
    strncpy(settings.scriptPerm, sp, 7); settings.scriptPerm[7] = 0;
  }
}



class WorldChangesReader {
  File& file;
  bool located;
  bool ended;
  bool endAfterValue;

  int nextNonSpace() {
    int c;
    do { c = file.read(); } while (c >= 0 && (c == ' ' || c == '\r' || c == '\n' || c == '\t'));
    return c;
  }

  bool locateChanges() {
    static const char token[] = "\"changes\"";
    char window[sizeof(token) - 1] = {};
    size_t filled = 0;
    while (file.available()) {
      int c = file.read();
      if (filled < sizeof(window)) window[filled++] = (char)c;
      else {
        memmove(window, window + 1, sizeof(window) - 1);
        window[sizeof(window) - 1] = (char)c;
      }
      if (filled == sizeof(window) && memcmp(window, token, sizeof(window)) == 0) {
        if (nextNonSpace() != ':') return false;
        return nextNonSpace() == '{';
      }
    }
    return false;
  }

public:
  explicit WorldChangesReader(File& input) : file(input), located(false), ended(false), endAfterValue(false) {}

  bool next(long& x, long& y, long& z, long& block) {
    if (ended) return false;
    if (!located) {
      located = true;
      if (!locateChanges()) { ended = true; return false; }
    }
    if (endAfterValue) { ended = true; return false; }
    int c = nextNonSpace();
    if (c == ',') c = nextNonSpace();
    if (c == '}') { ended = true; return false; }
    if (c != '"') { ended = true; return false; }

    char key[48];
    size_t keyLen = 0;
    bool keyOverflow = false;
    while (file.available()) {
      c = file.read();
      if (c == '"') break;
      if (keyLen + 1 < sizeof(key)) key[keyLen++] = (char)c;
      else keyOverflow = true;
    }
    key[keyLen] = 0;
    if (c != '"' || keyOverflow || nextNonSpace() != ':') { ended = true; return false; }

    char value[16];
    size_t valueLen = 0;
    c = nextNonSpace();
    while (c >= 0 && c != ',' && c != '}') {
      if (c != ' ' && c != '\r' && c != '\n' && c != '\t' && valueLen + 1 < sizeof(value))
        value[valueLen++] = (char)c;
      c = file.read();
    }
    value[valueLen] = 0;
    if (c == '}') endAfterValue = true;

    long coords[3];
    if (sscanf(key, "%ld,%ld,%ld", &coords[0], &coords[1], &coords[2]) != 3 || !valueLen) return false;
    x = coords[0]; y = coords[1]; z = coords[2]; block = strtol(value, nullptr, 10);
    return true;
  }
};
static World::Change* allocateChangeArray(int capacity) {
  size_t bytes = (size_t)capacity * sizeof(World::Change);
  if (ESP.getPsramSize()) {
    World::Change* external = (World::Change*)heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (external) {
      worldChangesInPsram = true;
      return external;
    }
  }
  World::Change* internal = (World::Change*)calloc((size_t)capacity, sizeof(World::Change));
  if (internal) worldChangesInPsram = false;
  return internal;
}

static bool allocateWorldChanges() {
  int capacity = maxChangeCapacity;
  while (capacity >= 256) {
    world.changes = allocateChangeArray(capacity);
    if (world.changes) {
      changeCapacity = capacity;
      break;
    }
    if (capacity > MAX_CHANGES) {
      int smaller = capacity / 2;
      capacity = smaller < MAX_CHANGES ? MAX_CHANGES : smaller;
    } else {
      capacity /= 2;
    }
  }
  if (!world.changes) {
    DBG("ERROR: Not enough RAM for world changes; free heap=%u bytes", (unsigned)ESP.getFreeHeap());
    return false;
  }
  Serial.printf("[WORLD] Change cache allocated: %d entries (%u bytes) in %s\r\n",
                changeCapacity, (unsigned)(changeCapacity * sizeof(World::Change)),
                worldChangesInPsram ? "PSRAM" : "heap");
  return true;
}
static bool ensureWorldChangeCache() {
  if (world.changes) return true;
  if (changeCapacity <= 0) return allocateWorldChanges();
  world.changes = allocateChangeArray(changeCapacity);
  if (!world.changes) {
    Serial.printf("[WORLD] Cannot allocate %d-entry RAM change cache; free heap=%u bytes\r\n", changeCapacity, (unsigned)ESP.getFreeHeap());
    return false;
  }
  return true;
}
static bool growWorldChangeCache() {
  if (!world.changes) return ensureWorldChangeCache();
  if (changeCapacity >= maxChangeCapacity) return false;
  int newCapacity = changeCapacity > 0 ? changeCapacity * 2 : 256;
  if (newCapacity > maxChangeCapacity) newCapacity = maxChangeCapacity;
  World::Change* grown = worldChangesInPsram
    ? (World::Change*)heap_caps_realloc(world.changes, (size_t)newCapacity * sizeof(World::Change), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    : (World::Change*)realloc(world.changes, (size_t)newCapacity * sizeof(World::Change));
  if (!grown) {
    Serial.printf("[WORLD] Could not grow RAM change cache from %d to %d entries; free heap=%u bytes\r\n",
                  changeCapacity, newCapacity, (unsigned)ESP.getFreeHeap());
    return false;
  }
  memset(grown + changeCapacity, 0, (size_t)(newCapacity - changeCapacity) * sizeof(World::Change));
  world.changes = grown;
  changeCapacity = newCapacity;
  Serial.printf("[WORLD] RAM change cache grew to %d entries\r\n", changeCapacity);
  return true;
}
static void loadWorld() {
  world.seed = serverConfig.seed;
  serverOwner[0] = 0;
  strncpy(world.worldType, serverConfig.worldType, 11);
  strncpy(world.mode, serverConfig.defaultMode, 11);
  world.nChanges = 0; world.nSigns = 0; world.time = 0.02; world.nBans = 0;
  strncpy(world.weather, "clear", 7);
  world.experimental = serverConfig.experimental;

  File f = storageFS->open(activeWorldFile, "r");
  if (!f) {
    Serial.printf("[WORLD] No %s file; using new-world defaults\r\n", activeWorldFile.c_str());
    return;
  }
  size_t worldFileSize = f.size();
  Serial.printf("[WORLD] Reading %s (%u bytes)\r\n", activeWorldFile.c_str(), (unsigned)worldFileSize);
  // A large changes map needs significantly more ArduinoJson memory than the
  // JSON file size. Grow the document to fit, while reserving heap for Wi-Fi
  // and the running server. Fail cleanly instead of exhausting startup RAM.
  const size_t heapReserve = 48 * 1024;
  size_t freeHeap = ESP.getFreeHeap();
  size_t maxDocCapacity = freeHeap > heapReserve ? freeHeap - heapReserve : 0;
  size_t requestedCapacity = worldFileSize <= SIZE_MAX / 2 ? worldFileSize * 2 : SIZE_MAX;
  if (requestedCapacity > maxDocCapacity) requestedCapacity = maxDocCapacity;
  if (requestedCapacity < 24576) {
    f.close();
    worldDirty = false; // preserve an existing file that could not be parsed safely
    Serial.printf("[WORLD] Not enough free heap to safely parse this world (need at least 24576 bytes, available=%u); keeping defaults\r\n", (unsigned)maxDocCapacity);
    return;
  }
  Serial.printf("[WORLD] JSON document capacity=%u bytes\r\n", (unsigned)requestedCapacity);
  DynamicJsonDocument doc(requestedCapacity);
  if (doc.capacity() < requestedCapacity) {
    f.close();
    worldDirty = false;
    Serial.printf("[WORLD] Could not allocate JSON document (capacity=%u); keeping defaults\r\n", (unsigned)doc.capacity());
    return;
  }
  Serial.println("[WORLD] Parsing with 1024-byte file reads...");
  ChunkedFileStream chunkedFile(f);
  DeserializationError err = deserializeJson(doc, chunkedFile);
  f.close();
  if (err) {
    worldDirty = false;
    Serial.printf("[WORLD] JSON parse failed: %s (doc usage=%u/%u); using defaults\r\n", err.c_str(), (unsigned)doc.memoryUsage(), (unsigned)doc.capacity());
    DBG("World load failed: %s; using defaults", err.c_str());
    return; // defaults above stay in place
  }
  if (!doc.is<JsonObject>()) {
    worldDirty = false;
    Serial.println("[WORLD] Root JSON value is not an object; using defaults");
    return;
  }
  world.seed = doc["seed"] | (long)serverConfig.seed;
  strncpy(world.worldType, doc["worldType"] | "default", 11); world.worldType[11] = 0;
  strncpy(world.mode, doc["mode"] | "survival", 11); world.mode[11] = 0;
  world.nChanges = 0; // block edits are streamed from world.json when needed

  JsonObject signs = doc["signs"];
  if (!signs.isNull()) {
    for (JsonPair kv : signs) {
      int x, y, z;
      if (sscanf(kv.key().c_str(), "%d,%d,%d", &x, &y, &z) != 3 || !kv.value().is<JsonArray>()) continue;
      char lines[4][16] = {};
      JsonArray row = kv.value().as<JsonArray>();
      for (int i = 0; i < 4 && i < (int)row.size(); i++) {
        const char* line = row[i] | "";
        strncpy(lines[i], line, 15); lines[i][15] = 0;
      }
      if (world.nSigns < MAX_SIGNS) {
        World::Sign& sign = world.signs[world.nSigns++];
        sign.x = x; sign.y = y; sign.z = z; memcpy(sign.lines, lines, sizeof(lines));
      }
    }
  }
  world.time = doc["time"] | 0.02;
  world.nBans = 0;
  JsonArray bans = doc["bans"];
  if (!bans.isNull()) for (JsonVariant entry : bans) {
    if (world.nBans >= MAX_BANS) break;
    const char* b = entry | (const char*)nullptr;
    if (!b || !isPlayerNameOk(b)) continue;
    strncpy(world.bans[world.nBans], b, 16);
    world.bans[world.nBans][16] = 0;
    for (char* c = world.bans[world.nBans]; *c; c++) *c = tolower((unsigned char)*c);
    world.nBans++;
  }
  const char* w = doc["weather"] | "clear";
  bool okW = false; for (auto x : WEATHERS) if (!strcmp(w, x)) okW = true;
  strncpy(world.weather, okW ? w : "clear", 7); world.weather[7] = 0;
  world.experimental = doc["experimental"] | false;
  const char* owner = doc["owner"] | "";
  if (isPlayerNameOk(owner)) {
    strncpy(serverOwner, owner, 16); serverOwner[16] = 0;
    for (char* c = serverOwner; *c; c++) *c = tolower((unsigned char) * c);
  }
  JsonObject st = doc["settings"];
  if (!st.isNull()) applySettingsJson(st);
  cleanRanks(doc["ranks"]);
  doc.clear();
  Serial.printf("[WORLD] Metadata loaded; RAM cache has %d pending edits, %d signs, %d bans\r\n", world.nChanges, world.nSigns, world.nBans);
  DBG("Loaded %s: seed %ld, %d pending edits", activeWorldFile.c_str(), world.seed, world.nChanges);
}

static double currentTime() {
  if (settings.alwaysDay) return 0.25;
  return fmod(timeBaseValue + (double)(millis() - timeBaseAt) / 1000.0 / DAY_LENGTH, 1.0);
}

static bool flushWorldChangeCache() {
  if (world.nChanges <= 0) return true;
  int flushed = world.nChanges;
  worldDirty = true;
  saveWorld();
  if (worldDirty) {
    Serial.printf("[WORLD] Flush failed: could not save %d edits to %s; keeping RAM cache\r\n", flushed, activeWorldFile.c_str());
    return false;
  }
  heap_caps_free(world.changes);
  worldChangesInPsram = false;
  world.changes = nullptr;
  world.nChanges = 0;
  changeCapacity = 0;
  Serial.printf("[WORLD] Flushed %d block edits into %s; RAM change cache freed\r\n", flushed, activeWorldFile.c_str());
  return true;
}
static void saveWorld() {
  if (!worldDirty) return;
  world.time = currentTime();
  File oldWorld = storageFS->open(activeWorldFile, "r");
  WorldChangesReader oldChanges(oldWorld);
  String tmp = activeWorldFile + ".tmp";
  File f = storageFS->open(tmp, "w");
  if (!f) {
    if (oldWorld) oldWorld.close();
    DBG("ERROR: saveWorld cannot open %s", tmp.c_str());
    return;
  }
  f.print("{");
  f.printf("\"seed\":%ld,", world.seed);
  f.printf("\"worldType\":\"%s\",", world.worldType);
  f.printf("\"mode\":\"%s\",", world.mode);
  f.print("\"changes\":{");
  bool firstChange = true;
  uint32_t mergedChangeCount = 0;
  long oldX, oldY, oldZ, oldBlock;
  while (oldWorld && oldChanges.next(oldX, oldY, oldZ, oldBlock)) {
    bool replaced = false;
    for (int i = 0; i < world.nChanges; i++) {
      if (world.changes[i].x == oldX && world.changes[i].y == oldY && world.changes[i].z == oldZ) {
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      if (!firstChange) f.print(",");
      f.printf("\"%ld,%ld,%ld\":%ld", oldX, oldY, oldZ, oldBlock);
      firstChange = false;
      mergedChangeCount++;
    }
    delay(0);
  }
  if (oldWorld) oldWorld.close();
  for (int i = 0; i < world.nChanges; i++) {
    if (!firstChange) f.print(",");
    f.printf("\"%ld,%ld,%ld\":%ld", (long)world.changes[i].x, (long)world.changes[i].y, (long)world.changes[i].z, (long)world.changes[i].b);
    firstChange = false;
    mergedChangeCount++;
  }
  f.print("},");
  f.print("\"signs\":{");
  for (int i = 0; i < world.nSigns; i++) {
    if (i) f.print(",");
    const World::Sign& sign = world.signs[i];
    f.printf("\"%ld,%ld,%ld\":[", (long)sign.x, (long)sign.y, (long)sign.z);
    for (int line = 0; line < 4; line++) {
      if (line) f.print(",");
      f.print(jsonEsc(sign.lines[line]));
    }
    f.print("]");
  }
  f.print("},");
  f.printf("\"time\":%.6f,", world.time);
  f.printf("\"settings\":{\"difficulty\":\"%s\",\"keepInventory\":%s,\"scripts\":%s,\"mods\":%s,\"commands\":%s,\"alwaysDay\":%s,\"weatherCycle\":%s,\"scriptPerm\":\"%s\"},",
           settings.difficulty, settings.keepInventory ? "true" : "false", settings.scripts ? "true" : "false",
           settings.mods ? "true" : "false", settings.commands ? "true" : "false",
           settings.alwaysDay ? "true" : "false", settings.weatherCycle ? "true" : "false", settings.scriptPerm);
  f.print("\"bans\":[");
  for (int i = 0; i < world.nBans; i++) {
    if (i) f.print(",");
    f.printf("\"%s\"", world.bans[i]);
  }
  f.print("],");
  f.printf("\"weather\":\"%s\",", world.weather);
  f.printf("\"experimental\":%s,", world.experimental ? "true" : "false");
  f.print("\"owner\":"); f.print(jsonEsc(serverOwner)); f.print(",");
  f.print("\"ranks\":");
  f.print(ranksJson());
  f.print("}");
  f.flush();
  bool tempWriteFailed = f.getWriteError() != 0;
  size_t tempSize = f.size();
  f.close();
  if (tempWriteFailed || tempSize == 0) {
    storageFS->remove(tmp);
    worldDirty = true;
    Serial.printf("[WORLD] ERROR: temporary world write failed (%u bytes, writeError=%d); keeping current file\r\n",
                  (unsigned)tempSize, tempWriteFailed ? 1 : 0);
    return;
  }

  // Keep the old world recoverable until the completed temporary file is in place.
  String backup = activeWorldFile + ".bak";
  storageFS->remove(backup);
  bool hadOldWorld = storageFS->exists(activeWorldFile);
  if (hadOldWorld && !storageFS->rename(activeWorldFile, backup)) {
    storageFS->remove(tmp);
    worldDirty = true;
    Serial.printf("[WORLD] ERROR: could not back up %s; keeping it unchanged\r\n", activeWorldFile.c_str());
    return;
  }
  if (!storageFS->rename(tmp, activeWorldFile)) {
    if (hadOldWorld) storageFS->rename(backup, activeWorldFile);
    storageFS->remove(tmp);
    worldDirty = true;
    Serial.printf("[WORLD] ERROR: could not install new %s; previous file restored\r\n", activeWorldFile.c_str());
    return;
  }
  if (hadOldWorld) storageFS->remove(backup);
  worldDirty = false;
  Serial.printf("[WORLD] Saved %lu block edits and %d signs into %s (%d pending RAM edits)\r\n",
                (unsigned long)mergedChangeCount, world.nSigns, activeWorldFile.c_str(), world.nChanges);
}

// ---------------- Players (forward decls) ----------------
struct Player;
struct WsConn;
static void playersRemove(Player* p);
static void broadcast(const String& obj, int exceptId = -1);
static void systemMsg(const String& text);
static void setWeather(const char* kind);
static bool isNight();
static void checkSleepers();
static void pickHost();
static void refreshHostRising();
static void sendBadge(Player* p);
static void handle(Player* p, JsonDocument& msg);
static void handleAdmin(Player* p, JsonDocument& msg);

// ---------------- Mutes ----------------
static char mutes[MAX_MUTES][17];
static int nMutes = 0;
static bool isMuted(const char* lowName) {
  for (int i = 0; i < nMutes; i++) if (!strcmp(mutes[i], lowName)) return true;
  return false;
}

// ---------------- Supabase (admins, verified names, standing, global bans) ----------------
static String sbRequest(const char* pathAndQuery, const char* token, const char* method, const char* body) {
  WiFiClientSecure client;
  client.setInsecure(); // Node verifies the CA chain; on ESP32 we skip CA pinning for size
  HTTPClient http;
  String url = String(serverConfig.supabaseUrl) + pathAndQuery;
  if (!http.begin(client, url)) return "";
  http.setTimeout(4000);
  String auth = String("Bearer ") + (token && strlen(token) ? token : serverConfig.supabaseAnonKey);
  http.addHeader("apikey", serverConfig.supabaseAnonKey);
  http.addHeader("authorization", auth);
  http.addHeader("content-type", "application/json");
  int code = (method && strcmp(method, "POST") == 0) ? http.POST(body ? body : "") : http.GET();
  String out = "";
  if (code == 200) out = http.getString();
  http.end();
  return out;
}

struct CacheEntry {
  String key;
  bool ok;
  String str;
  uint32_t until;
};
static CacheEntry adminCache[32]; static int nAdminCache = 0;
static CacheEntry whoCache[32]; static int nWhoCache = 0;
static CacheEntry standingCache[32]; static int nStandingCache = 0;

static CacheEntry* cacheGet(CacheEntry* c, int n, const String& k) {
  for (int i = 0; i < n; i++) if (c[i].key == k && (int32_t)(c[i].until - millis()) > 0) return &c[i];
  return nullptr;
}
static CacheEntry* cachePut(CacheEntry* c, int& n, int cap, const String& k) {
  if (n >= cap) n = 0; // clear-when-full, like the Node caches
  CacheEntry* e = &c[n++];
  e->key = k;
  return e;
}

// Staff (is_staff) or admin (is_admin) may use /panel actions
static bool isAdminToken(const String& token) {
  if (token.length() < 20 || token.length() > 4096) return false;
  CacheEntry* hit = cacheGet(adminCache, nAdminCache, token);
  if (hit) return hit->ok;
  String r = sbRequest("/rest/v1/rpc/is_staff", token.c_str(), "POST", "{}");
  r.trim();
  bool ok = (r == "true");
  if (r.length() == 0) { // older databases only have is_admin
    r = sbRequest("/rest/v1/rpc/is_admin", token.c_str(), "POST", "{}");
    r.trim();
    ok = (r == "true");
  }
  CacheEntry* e = cachePut(adminCache, nAdminCache, 32, token);
  e->ok = ok; e->until = millis() + 60000;
  return ok;
}

// Who is behind an access token? Returns the VibeBlox username of that
// (email-verified) account, or "" — stops anyone joining as someone else's name.
static String verifiedName(const String& token) {
  if (token.length() < 20 || token.length() > 4096) return "";
  CacheEntry* hit = cacheGet(whoCache, nWhoCache, token);
  if (hit) return hit->str;
  String name = "";
  String user = sbRequest("/auth/v1/user", token.c_str(), "GET", nullptr);
  if (user.length()) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, user)) {
      const char* id = doc["id"] | (const char*)nullptr;
      const char* confirmedAt = doc["email_confirmed_at"] | (const char*)nullptr;
      bool confirmed = confirmedAt && strlen(confirmedAt) > 0;
      if (id && strlen(id) == 36 && confirmed) {
        String q = String("/rest/v1/profiles?id=eq.") + id + "&select=username&limit=1";
        String rows = sbRequest(q.c_str(), token.c_str(), "GET", nullptr);
        DynamicJsonDocument d2(1024);
        if (!deserializeJson(d2, rows) && d2.is<JsonArray>() && d2.size() > 0) {
          const char* un = d2[0]["username"] | (const char*)nullptr;
          if (un && isPlayerNameOk(un)) name = un;
        }
      }
    }
  }
  CacheEntry* e = cachePut(whoCache, nWhoCache, 32, token);
  e->str = name; e->until = millis() + 300000;
  return name;
}

struct Standing {
  long plays;
  Tier tier;
  bool rising;
  char rank[16];
  bool og;
  char staff[8];
  char special[8];
};
static bool standingOf(const String& name, Standing& out) {
  out.plays = 0; out.tier = T_NONE; out.rising = false; out.og = false;
  strcpy(out.rank, "none"); out.staff[0] = 0; out.special[0] = 0;
  String r = sbRequest("/rest/v1/rpc/player_standing", nullptr, "POST",
                       (String("{\"p_username\":\"") + name + "\"}").c_str());
  if (r.length()) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, r) && doc.is<JsonArray>() && doc.size() > 0) {
      JsonObject row = doc[0];
      out.plays = row["plays"] | 0;
      out.tier = tierOfPlays(out.plays);
      out.rising = row["rising"] | false;
      const char* rk = row["rank"] | "none";
      strncpy(out.rank, rk, 15); out.rank[15] = 0;
      out.og = (row["og"] | false) == true;
      const char* st = row["staff"] | "";
      if (!strcmp(st, "helper") || !strcmp(st, "mod")) strncpy(out.staff, st, 7);
      const char* special = row["special"] | "";
      if (!strcmp(special, "youtube")) strcpy(out.special, "youtube");
    }
  }
  // pack into cache: plays|tier|rising|rank|og|staff|special
  String key = name; key.toLowerCase();
  CacheEntry* hit = cacheGet(standingCache, nStandingCache, key);
  if (!hit) hit = cachePut(standingCache, nStandingCache, 32, key);
  char buf[80];
  snprintf(buf, sizeof(buf), "%ld|%d|%d|%s|%d|%s|%s", out.plays, (int)out.tier, out.rising ? 1 : 0, out.rank, out.og ? 1 : 0, out.staff[0] ? out.staff : "-", out.special[0] ? out.special : "-");
  hit->str = buf; hit->until = millis() + 300000;
  return true;
}
static bool standingFromCache(const String& name, Standing& out) {
  String key = name; key.toLowerCase();
  CacheEntry* hit = cacheGet(standingCache, nStandingCache, key);
  if (!hit) return false;
  char buf[80]; strncpy(buf, hit->str.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char rank[16] = "", staff[8] = "", special[8] = "";
  long plays; int tier, rising, og;
  int fields = sscanf(buf, "%ld|%d|%d|%15[^|]|%d|%7[^|]|%7s", &plays, &tier, &rising, rank, &og, staff, special);
  if (fields < 5) return false;
  out.plays = plays; out.tier = (Tier)tier; out.rising = rising == 1;
  strncpy(out.rank, rank, 15); out.rank[15] = 0;
  out.og = og == 1; strncpy(out.staff, staff, 7); out.staff[7] = 0;
  out.special[0] = 0;
  if (strcmp(out.staff, "-") == 0) out.staff[0] = 0;
  if (fields >= 7 && !strcmp(special, "youtube")) strcpy(out.special, "youtube");
  return true;
}

// A global ban keeps a name out of every server. Fails open if Supabase is down.
static bool isGloballyBanned(const String& name) {
  String low = name; low.toLowerCase();
  String q = String("/rest/v1/bans?username=eq.") + low + "&select=username&limit=1";
  String r = sbRequest(q.c_str(), nullptr, "GET", nullptr);
  return r.length() > 2; // "[]" means no rows
}

// ---------------- Minimal WebSocket (RFC 6455) over raw TCP ----------------
struct Player {
  int id;
  struct WsConn* conn;
  char name[17];
  bool joined, joining, verified, moved;
  float x, y, z, yaw, pitch;
  char mode[13];
  int held, act;
  char skin[25], cape[25];
  char* custom; // custom skin data URI, heap
  char* customCape; // custom cape data URI, heap
  struct {
    bool owner;
    char rank[16];
    char tier[8];
    bool og;
    char staff[8];
    char special[8];
  } badge;
  uint32_t joinedAt;
  uint32_t badgeAt;
  int msgsThisSecond, blocksThisSecond, dropsThisSecond;
  uint32_t sleepSince;
};

struct WsConn {
  WiFiClient sock;
  uint8_t* buf = nullptr;
  size_t len = 0, cap = 0;
  String frag;           // fragment reassembly
  bool closed = false;
  uint32_t lastSeen = 0;
  Player* p = nullptr;

  void feed(const uint8_t* data, size_t n);
  void sendRaw(uint8_t op, const uint8_t* payload, size_t n);
  void send(const String& obj);
  void ping() {
    sendRaw(0x9, nullptr, 0);
  }
  void close(uint16_t code = 1000, const char* reason = "");
  bool ensureCap(size_t need);
};

static WsConn** conns = nullptr;
static Player* players = nullptr;
static int nConns = 0;

static bool allocateRuntimeCapacity() {
  uint64_t psramBytes = ESP.getPsramSize();
  uint64_t playerSlots = (uint64_t)MAX_WS_PLAYERS + psramBytes / 32768ULL;
  uint64_t editSlots = (uint64_t)MAX_CHANGES + psramBytes / 32ULL;
  if (playerSlots > INT_MAX || editSlots > INT_MAX) {
    Serial.println("[BOOT] ERROR: detected PSRAM exceeds supported runtime capacity");
    return false;
  }
  maxWsPlayers = (int)playerSlots;
  maxChangeCapacity = (int)editSlots;

  size_t playerBytes = (size_t)maxWsPlayers * sizeof(Player);
  if (psramBytes) players = (Player*)heap_caps_calloc(1, playerBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!players) players = (Player*)calloc((size_t)maxWsPlayers, sizeof(Player));
  conns = (WsConn**)calloc((size_t)maxWsPlayers, sizeof(WsConn*));
  if (!players || !conns) {
    if (players) heap_caps_free(players);
    if (conns) free(conns);
    players = nullptr;
    conns = nullptr;
    Serial.printf("[BOOT] ERROR: cannot allocate slots for %d players; free heap=%u PSRAM=%u\r\n",
                  maxWsPlayers, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    return false;
  }
  Serial.printf("[BOOT] Runtime capacity: %d player slots, %d cached block edits (PSRAM=%u bytes)\r\n",
                maxWsPlayers, maxChangeCapacity, (unsigned)psramBytes);
  return true;
}

bool WsConn::ensureCap(size_t need) {
  if (need <= cap) return true;
  size_t nc = cap ? cap : 2048;
  while (nc < need) nc *= 2;
  if (nc > MAX_FRAME + 4096) return false;
  uint8_t* nb = (uint8_t*)realloc(buf, nc);
  if (!nb) return false;
  buf = nb; cap = nc;
  return true;
}

void WsConn::feed(const uint8_t* data, size_t n) {
  lastSeen = millis();
  if (closed) return;
  if (!ensureCap(len + n)) return close(1009, "Message too big");
  memcpy(buf + len, data, n);
  len += n;
  size_t off = 0;
  while (len - off >= 2) {
    uint8_t b0 = buf[off], b1 = buf[off + 1];
    bool fin = (b0 & 0x80) != 0;
    uint8_t op = b0 & 0x0f;
    bool masked = (b1 & 0x80) != 0;
    uint64_t flen = b1 & 0x7f;
    size_t hoff = 2;
    if (flen == 126) {
      if (len - off < 4) break;
      flen = ((uint64_t)buf[off + 2] << 8) | buf[off + 3];
      hoff = 4;
    } else if (flen == 127) {
      if (len - off < 10) break;
      flen = 0;
      for (int i = 0; i < 8; i++) flen = (flen << 8) | buf[off + 2 + i];
      hoff = 10;
    }
    if (flen > MAX_FRAME) return close(1009, "Message too big");
    if (!masked) return close(1002, "Client frames must be masked");
    if (len - off < hoff + 4 + flen) break; // wait for the rest
    const uint8_t* mask = buf + off + hoff;
    for (uint64_t i = 0; i < flen; i++) buf[off + hoff + 4 + i] ^= mask[i & 3]; // unmask in place
    const uint8_t* payload = buf + off + hoff + 4;
    size_t consumed = hoff + 4 + flen;

    if (op == 0x8) return close(1000, "");
    if (op == 0x9) sendRaw(0xa, payload, flen); // ping -> pong
    else if (op == 0xa) {
      /* pong */
    }
    else if (op == 0x1 || op == 0x0) {
      frag.concat((const char*)payload, flen);
      if (frag.length() > MAX_FRAME) return close(1009, "Message too big");
      if (fin) {
        String text = frag;
        frag = "";
        if (p) {
          Player* pl = p;
          if (++pl->msgsThisSecond > 120) return close(4002, "Sending too fast");
          DynamicJsonDocument doc(MAX_FRAME);
          DeserializationError err = deserializeJson(doc, text);
          if (!err && doc.is<JsonObject>()) handle(pl, doc);
        }
      }
    }
    // binary frames (0x2) are ignored
    off += consumed;
  }
  if (off > 0) {
    memmove(buf, buf + off, len - off);
    len -= off;
  }
}

void WsConn::sendRaw(uint8_t op, const uint8_t* payload, size_t n) {
  if (closed) return;
  uint8_t header[10];
  size_t hl;
  if (n < 126) {
    header[0] = 0x80 | op;
    header[1] = (uint8_t)n;
    hl = 2;
  }
  else if (n < 65536) {
    header[0] = 0x80 | op;
    header[1] = 126;
    header[2] = (n >> 8) & 0xff;
    header[3] = n & 0xff;
    hl = 4;
  }
  else {
    header[0] = 0x80 | op;
    header[1] = 127;
    for (int i = 0; i < 8; i++) header[2 + i] = (n >> (8 * (7 - i))) & 0xff;
    hl = 10;
  }
  sock.write(header, hl);
  if (n) sock.write(payload, n);
}

void WsConn::send(const String& obj) {
  sendRaw(0x1, (const uint8_t*)obj.c_str(), obj.length());
}

void WsConn::close(uint16_t code, const char* reason) {
  if (closed) return;
  DBG("WebSocket closing: code=%u reason=%s", code, reason);
  uint8_t pl[2 + 100];
  size_t rl = strlen(reason); if (rl > 100) rl = 100;
  pl[0] = code >> 8; pl[1] = code & 0xff;
  memcpy(pl + 2, reason, rl);
  sendRaw(0x8, pl, 2 + rl);
  sock.stop();
  closed = true;
  if (p) {
    Player* closingPlayer = p;
    p = nullptr;
    playersRemove(closingPlayer);
  }
}

// ---------------- Players ----------------

static int savedPlayerIndex(const char* name) {
  char key[17];
  strncpy(key, name ? name : "", 16); key[16] = 0;
  for (char* c = key; *c; c++) *c = tolower((unsigned char) * c);
  for (int i = 0; i < nSavedPlayers; i++) if (!strcmp(key, savedPlayers[i].name)) return i;
  return -1;
}

static void rememberPlayer(Player* p) {
  if (!p || !isPlayerNameOk(p->name)) return;
  int i = savedPlayerIndex(p->name);
  if (i < 0) {
    if (nSavedPlayers < MAX_SAVED_PLAYERS) i = nSavedPlayers++;
    else {
      i = 0;
      for (int j = 1; j < nSavedPlayers; j++) if ((int32_t)(savedPlayers[j].updated - savedPlayers[i].updated) < 0) i = j;
    }
  }
  SavedPlayerData& d = savedPlayers[i];
  strncpy(d.name, p->name, 16); d.name[16] = 0;
  for (char* c = d.name; *c; c++) *c = tolower((unsigned char) * c);
  d.x = p->x; d.y = p->y; d.z = p->z; d.yaw = p->yaw; d.pitch = p->pitch;
  strncpy(d.mode, p->mode, 12); d.mode[12] = 0;
  d.held = p->held;
  d.updated = millis();
  playerDataDirty = true;
}

static void restorePlayer(Player* p) {
  if (!p) return;
  int i = savedPlayerIndex(p->name);
  if (i < 0) return;
  const SavedPlayerData& d = savedPlayers[i];
  p->x = d.x; p->y = d.y; p->z = d.z; p->yaw = d.yaw; p->pitch = d.pitch;
  strncpy(p->mode, d.mode, 12); p->mode[12] = 0;
  p->held = d.held;
  p->moved = true;
}
static int hostId = -1; // the host runs the server: the first player in, then the longest-connected one
static int nextId = 1;

static Player* playerById(int id) {
  for (int i = 0; i < maxWsPlayers; i++) if (players[i].conn && players[i].id == id) return &players[i];
  return nullptr;
}
static Player* playerByName(const String& lowName) {
  for (int i = 0; i < maxWsPlayers; i++) {
    if (!players[i].conn || !players[i].joined) continue;
    String n = players[i].name; n.toLowerCase();
    if (n == lowName) return &players[i];
  }
  return nullptr;
}
static int joinedCount() {
  int n = 0;
  for (int i = 0; i < maxWsPlayers; i++) if (players[i].conn && players[i].joined) n++;
  return n;
}

// Slot is released but stays owned by the (dead) player record until reused.
static void playersRemove(Player* p) {
  if (!p) return;
  bool wasJoined = p->joined;
  int playerId = p->id;
  String playerName = p->name;

  if (p->custom) {
    free(p->custom);
    p->custom = nullptr;
  }
  if (p->customCape) {
    free(p->customCape);
    p->customCape = nullptr;
  }
  p->joined = false;
  p->joining = false;
  p->conn = nullptr; // the WsConn itself is reaped by the main loop

  if (wasJoined) {
    DBG("Player disconnected: id=%d name=%s", playerId, playerName.c_str());
    rememberPlayer(p);
    savePlayerData();
    broadcast(String("{\"t\":\"leave\",\"id\":") + playerId + ",\"name\":" + jsonEsc(playerName) + "}");
    systemMsg(playerName + " left the game");
    if (playerId == hostId) pickHost();
    checkSleepers();
    DBG("Player left: %s", playerName.c_str());
  }
}

static void broadcast(const String& obj, int exceptId) {
  for (int i = 0; i < maxWsPlayers; i++) {
    Player* p = &players[i];
    if (p->conn && p->joined && p->id != exceptId && !p->conn->closed) p->conn->send(obj);
  }
}
static void systemMsg(const String& text) {
  broadcast(String("{\"t\":\"system\",\"text\":") + jsonEsc(text) + "}");
}

static void cleanName(const char* raw, int id, bool verified, char out[17]) {
  String name = raw ? String(raw) : "";
  String clean;
  for (size_t i = 0; i < name.length() && clean.length() < 16; i++) {
    char c = name[i];
    if (isalnum((unsigned char)c) || c == '_') clean += c;
  }
  if (clean.length() < 3) clean = String("Player") + id;
  if (!verified) {
    String lc = clean; lc.toLowerCase();
    if (lc == "master") clean = String("Player") + id; // only the signed-in owner may use this name
  }
  String finalName = clean;
  int n = 2;
  for (;;) {
    bool taken = false;
    for (int i = 0; i < maxWsPlayers; i++) {
      if (!players[i].conn || !players[i].joined) continue;
      String tn = players[i].name; tn.toLowerCase();
      String fl = finalName; fl.toLowerCase();
      if (tn == fl) {
        taken = true;
        break;
      }
    }
    if (!taken) break;
    finalName = clean.substring(0, 13) + String(n++);
  }
  strncpy(out, finalName.c_str(), 16); out[16] = 0;
}

// Thrown items still on the ground (shared id -> when thrown). Expire after 5 minutes.
struct Drop {
  char n[17];
  uint32_t at;
};
static Drop dropsArr[600];
static int nDrops = 0;
static int dropFind(const char* n) {
  for (int i = 0; i < nDrops; i++) if (!strcmp(dropsArr[i].n, n)) return i;
  return -1;
}

static bool validBlock(long x, long y, long z, long b) {
  return y >= 0 && y < 64 && labs(x) < 30000000L && labs(z) < 30000000L &&
         ((b >= 0 && b < 100) || (b >= 1000 && b < 1400) || (b >= 2000 && b < 2560));
}

static void worldSetChange(int x, int y, int z, int b) {
  for (int i = 0; i < world.nChanges; i++) {
    if (world.changes[i].x == x && world.changes[i].y == y && world.changes[i].z == z) {
      world.changes[i].b = b;
      return;
    }
  }

  if (!ensureWorldChangeCache()) return;
  if (world.nChanges >= changeCapacity && !growWorldChangeCache()) {
    if (!flushWorldChangeCache() || !ensureWorldChangeCache()) {
      Serial.println("[WORLD] Block edit deferred: unable to grow or flush RAM cache");
      return;
    }
  }
  world.changes[world.nChanges].x = x; world.changes[world.nChanges].y = y;
  world.changes[world.nChanges].z = z; world.changes[world.nChanges].b = b;
  world.nChanges++;
}

static void pickHost() {
  // earliest joinedAt among the joined players
  Player* best = nullptr;
  for (int i = 0; i < maxWsPlayers; i++) {
    Player* p = &players[i];
    if (!p->conn || !p->joined) continue;
    if (!best || p->joinedAt < best->joinedAt) best = p;
  }
  hostId = best ? best->id : -1;
  if (hostId != -1) {
    broadcast(String("{\"t\":\"host\",\"id\":") + hostId + "}");
    systemMsg(String(best->name) + " is now the host");
    refreshHostRising();
  }
}

static void sendJoinBlockChange(Player* p, int x, int y, int z, int b, String& batch, int& count) {
  if (count == 0) batch = "{\"t\":\"blocks\",\"id\":0,\"list\":[";
  else batch += ",";
  batch += String("[") + x + "," + y + "," + z + "," + b + "]";
  if (++count >= JOIN_BLOCK_BATCH) {
    batch += "]}";
    p->conn->send(batch);
    batch = "";
    count = 0;
    delay(0);
  }
}

static void sendJoinWorldChanges(Player* p) {
  String batch;
  int count = 0;
  uint32_t persistedCount = 0;
  File worldFile = storageFS->open(activeWorldFile, "r");
  if (worldFile) {
    WorldChangesReader storedChanges(worldFile);
    long x, y, z, block;
    while (storedChanges.next(x, y, z, block)) {
      sendJoinBlockChange(p, (int)x, (int)y, (int)z, (int)block, batch, count);
      persistedCount++;
      delay(0);
    }
    worldFile.close();
  }
  Serial.printf("[WORLD] Replayed %lu edits from %s to joining player\r\n", (unsigned long)persistedCount, activeWorldFile.c_str());
  for (int i = 0; i < world.nChanges; i++)
    sendJoinBlockChange(p, world.changes[i].x, world.changes[i].y,
                        world.changes[i].z, world.changes[i].b, batch, count);
  if (count) {
    batch += "]}";
    p->conn->send(batch);
  }
}

static void finishJoin(Player* p) {
  if (joinedCount() >= maxPlayers()) return p->conn->close(4001, "Server is full");
  bool assignedServerOwner = false;
  if (!serverOwner[0]) {
    strncpy(serverOwner, p->name, 16); serverOwner[16] = 0;
    for (char* c = serverOwner; *c; c++) *c = tolower((unsigned char) * c);
    assignedServerOwner = true;
    worldDirty = true;
    DBG("Assigned first player %s as server owner", p->name);
  }
  String joiningName = p->name; joiningName.toLowerCase();
  p->badge.owner = joiningName == serverOwner;
  p->joined = true;
  p->joinedAt = millis();
  bool becameHost = hostId == -1 || !playerById(hostId) || !playerById(hostId)->joined;
  if (becameHost) {
    hostId = p->id;
    refreshHostRising();
  }
  const char* espModel = ESP.getChipModel();
  String joinNotice = String("Powered by ESPBlox v1.b2 on ") + (espModel ? espModel : "ESP32");
  String others = "[";
  bool first = true;
  for (int i = 0; i < maxWsPlayers; i++) {
    Player* o = &players[i];
    if (!o->conn || !o->joined || o->id == p->id) continue;
    if (!first) others += ",";
    first = false;
    String look = String("{\"skin\":") + jsonEsc(o->skin) + ",\"cape\":" + jsonEsc(o->cape);
    if (o->custom) look += ",\"custom\":" + jsonEsc(o->custom);
    if (o->customCape) look += ",\"capeData\":" + jsonEsc(o->customCape);
    look += "}";
    others += String("{\"id\":") + o->id + ",\"name\":" + jsonEsc(o->name) +
              ",\"x\":" + String(o->x, 2) + ",\"y\":" + String(o->y, 2) + ",\"z\":" + String(o->z, 2) +
              ",\"yaw\":" + String(o->yaw, 3) + ",\"pitch\":" + String(o->pitch, 3) +
              ",\"mode\":" + jsonEsc(o->mode) + ",\"look\":" + look +
              ",\"badge\":{\"owner\":" + (o->badge.owner ? "true" : "false") + ",\"rank\":" + jsonEsc(o->badge.rank) +
              ",\"tier\":" + jsonEsc(o->badge.tier) + ",\"og\":" + (o->badge.og ? "true" : "false") +
              ",\"staff\":" + jsonEsc(o->badge.staff) + ",\"special\":" +
              (o->badge.special[0] ? jsonEsc(o->badge.special) : String("null")) + "}}";
  }
  others += "]";

  String signs = "[";
  for (int i = 0; i < world.nSigns; i++) {
    if (i) signs += ",";
    const World::Sign& sign = world.signs[i];
    signs += String("[") + sign.x + "," + sign.y + "," + sign.z + ",[";
    for (int line = 0; line < 4; line++) {
      if (line) signs += ",";
      signs += jsonEsc(sign.lines[line]);
    }
    signs += "]]";
  }
  signs += "]";

  String w = String("{\"t\":\"welcome\",\"id\":") + p->id + ",\"name\":" + jsonEsc(p->name) +
             ",\"playerState\":{\"x\":" + String(p->x, 4) + ",\"y\":" + String(p->y, 4) + ",\"z\":" + String(p->z, 4) + ",\"yaw\":" + String(p->yaw, 4) + ",\"pitch\":" + String(p->pitch, 4) + ",\"mode\":" + jsonEsc(p->mode) + ",\"held\":" + p->held + "}" +
             ",\"seed\":" + world.seed +
             ",\"worldType\":" + jsonEsc(world.worldType) +
             ",\"mode\":" + jsonEsc(world.mode) +
             ",\"time\":" + String(currentTime(), 6) +
             ",\"changes\":[]" +
             ",\"signs\":" + signs +
             ",\"players\":" + others +
             ",\"hostId\":" + hostId +
             ",\"settings\":{\"difficulty\":" + jsonEsc(settings.difficulty) +
             ",\"keepInventory\":" + (settings.keepInventory ? "true" : "false") +
             ",\"scripts\":" + (settings.scripts ? "true" : "false") +
             ",\"mods\":" + (settings.mods ? "true" : "false") +
             ",\"commands\":" + (settings.commands ? "true" : "false") +
             ",\"alwaysDay\":" + (settings.alwaysDay ? "true" : "false") +
             ",\"weatherCycle\":" + (settings.weatherCycle ? "true" : "false") +
             ",\"scriptPerm\":" + jsonEsc(settings.scriptPerm) + "}" +
             ",\"weather\":" + jsonEsc(world.weather) +
             ",\"experimental\":" + (world.experimental ? "true" : "false") +
             ",\"maxPlayers\":" + maxPlayers() +
             ",\"ranks\":" + ranksJson() + "}";
  p->conn->send(w);
  // Stream edits from disk and RAM in bounded WebSocket messages.
  sendJoinWorldChanges(p);
  p->conn->send(String("{\"t\":\"system\",\"text\":") + jsonEsc(joinNotice) + "}");
  if (safeMode) {
    p->conn->send("{\"t\":\"system\",\"text\":\"This server is in Safe Mode. You are playing in a temporary world; its changes will be deleted when the ESP32 reboots.\"}");
  }
  broadcast(String("{\"t\":\"join\",\"id\":") + p->id + ",\"name\":" + jsonEsc(p->name) + "}", p->id);
  sendBadge(p);
  if (assignedServerOwner) saveWorld(); // persist the initial owner immediately
  systemMsg(String(p->name) + " joined the game");
  DBG("Player joined: %s", p->name);
}

// The night matches the game: from dusk (0.53) to just before sunrise (0.97)
static bool isNight() {
  if (settings.alwaysDay) return false;
  double t = currentTime();
  return t > 0.53 && t < 0.97;
}

// ---------------- Badges / host tier ----------------
static void refreshHostRising() {
  Player* h = hostId == -1 ? nullptr : playerById(hostId);
  if (!h || !h->verified) {
    hostTier = T_NONE;
    return;
  }
  String ln = h->name; ln.toLowerCase();
  if (ln == serverOwner) {
    hostTier = T_STAR;
    return;
  }
  Standing st;
  if (standingFromCache(h->name, st) || standingOf(h->name, st)) hostTier = st.tier;
  else hostTier = T_NONE;
}

// What shows in front of a signed-in player's name: owner, paid rank, creator tier. Everyone is told.
static void sendBadge(Player* p) {
  Standing st = {};
  if (p->verified && !standingFromCache(p->name, st)) standingOf(p->name, st);
  if (p->conn->closed) return;
  String ln = p->name; ln.toLowerCase();
  bool owner = (ln == serverOwner);
  strncpy(p->badge.rank, owner ? "owner" : st.rank, 15); p->badge.rank[15] = 0;
  strncpy(p->badge.tier, owner ? "star" : tierName(st.tier), 7); p->badge.tier[7] = 0;
  p->badge.owner = owner;
  p->badge.og = st.og;
  strncpy(p->badge.staff, st.staff, 7); p->badge.staff[7] = 0;
  strncpy(p->badge.special, st.special, 7); p->badge.special[7] = 0;
  broadcast(String("{\"t\":\"badge\",\"id\":") + p->id +
            ",\"owner\":" + (owner ? "true" : "false") +
            ",\"rank\":" + jsonEsc(p->badge.rank) +
            ",\"tier\":" + jsonEsc(p->badge.tier) +
            ",\"og\":" + (p->badge.og ? "true" : "false") +
            ",\"staff\":" + jsonEsc(p->badge.staff) +
            ",\"special\":" + (p->badge.special[0] ? jsonEsc(p->badge.special) : String("null")) + "}");
}

// ---------------- Weather ----------------
static uint32_t weatherChangeAt = 0;
static uint32_t nextWeatherDelay(const char* kind) {
  uint32_t lo = !strcmp(kind, "clear") ? 420 : 180;
  uint32_t hi = !strcmp(kind, "clear") ? 1020 : 420;
  return lo + esp_random() % (hi - lo);
}
static void setWeather(const char* kind) {
  strncpy(world.weather, kind, 7); world.weather[7] = 0;
  worldDirty = true;
  weatherChangeAt = millis() + nextWeatherDelay(kind) * 1000;
  broadcast(String("{\"t\":\"weather\",\"kind\":") + jsonEsc(kind) + "}");
}

// ---------------- Admin (/panel) ----------------
static void handleAdmin(Player* p, JsonDocument& msg) {
  JsonObject o = msg.as<JsonObject>();
  auto reply = [&](const String & text) {
    p->conn->send(String("{\"t\":\"system\",\"text\":") + jsonEsc(text) + "}");
  };
  String token = o["token"] | "";
  if (!isAdminToken(token)) return reply("You are not an admin");
  const char* nameC = o["name"] | "";
  String name = nameC; name.toLowerCase();
  Player* target = name.length() ? playerByName(name) : nullptr;
  const char* action = o["action"] | "";
  if (!strcmp(action, "check")) return p->conn->send("{\"t\":\"adminOk\"}");
  if (!strcmp(action, "players")) {
    String list = "[";
    bool f = true;
    for (int i = 0; i < maxWsPlayers; i++) {
      Player* pl = &players[i];
      if (!pl->conn || !pl->joined) continue;
      if (!f) list += ",";
      f = false;
      list += String("{\"id\":") + pl->id + ",\"name\":" + jsonEsc(pl->name) + ",\"mode\":" + jsonEsc(pl->mode) + "}";
    }
    return p->conn->send(String("{\"t\":\"adminPlayers\",\"list\":") + list + "]}");
  }
  if (!strcmp(action, "kick") || !strcmp(action, "ban")) {
    if (!target) return reply(String("No player named ") + String(nameC).substring(0, 16) + " on this server");
    if (target->id == p->id || target->badge.owner) return reply("You can't remove that player");
    if (!strcmp(action, "ban")) {
      String n = target->name; n.toLowerCase();
      bool have = false;
      for (int i = 0; i < world.nBans; i++) if (!strcmp(world.bans[i], n.c_str())) {
          have = true;
          break;
        }
      if (!have && world.nBans < MAX_BANS) {
        strncpy(world.bans[world.nBans++], n.c_str(), 16);
        world.bans[world.nBans - 1][16] = 0;
        worldDirty = true; saveWorld();
      }
    }
    systemMsg(String(target->name) + (!strcmp(action, "ban") ? " was banned" : " was kicked") + " by an admin");
    target->conn->close(!strcmp(action, "ban") ? 4005 : 4004,
                        !strcmp(action, "ban") ? "You were banned by an admin" : "You were kicked by an admin");
    return;
  }
  if (!strcmp(action, "mute") || !strcmp(action, "unmute")) {
    if (!name.length()) return reply(String("Usage: /") + action + " <player>");
    if (!strcmp(action, "mute")) {
      if (!target) return reply(String("No player named ") + String(nameC).substring(0, 16) + " on this server");
      if (target->id == p->id || target->badge.owner) return reply("You can't mute that player");
      String n = target->name; n.toLowerCase();
      if (nMutes < MAX_MUTES) {
        strncpy(mutes[nMutes++], n.c_str(), 16);
        mutes[nMutes - 1][16] = 0;
      }
      systemMsg(String(target->name) + " was muted by " + p->name);
      target->conn->send("{\"t\":\"system\",\"text\":\"You were muted. Nobody can see your messages.\"}");
    } else {
      bool found = false;
      for (int i = 0; i < nMutes; i++) if (!strcmp(mutes[i], name.c_str())) {
          memmove(mutes[i], mutes[i + 1], sizeof(mutes[0]) * (nMutes - i - 1));
          nMutes--;
          found = true; break;
        }
      if (!found) return reply(String(String(nameC).substring(0, 16)) + " isn't muted");
      systemMsg(String(String(nameC).substring(0, 16)) + " can chat again");
    }
    return;
  }
  if (!strcmp(action, "mutes")) {
    String list;
    for (int i = 0; i < nMutes; i++) {
      if (list.length()) list += ", ";
      list += mutes[i];
    }
    return reply(list.length() ? String("Muted here: ") + list : "Nobody is muted on this server");
  }
  if (!strcmp(action, "gamemode")) {
    const char* mode = o["mode"] | "";
    bool ok = !strcmp(mode, "spectator");
    for (auto m : VALID_MODES) if (!strcmp(mode, m)) ok = true;
    if (!ok) return reply("Unknown game mode");
    Player* who = target ? target : (name.length() ? nullptr : p);
    if (!who) return reply(String("No player named ") + String(nameC).substring(0, 16) + " on this server");
    who->conn->send(String("{\"t\":\"setmode\",\"mode\":") + jsonEsc(mode) + ",\"by\":" + jsonEsc(p->name) + "}");
    if (who != p) reply(String(who->name) + " is now in " + mode + " mode");
    return;
  }
}

// ---------------- Message handling ----------------
static void handle(Player* p, JsonDocument& msg) {
  JsonObject o = msg.as<JsonObject>();
  const char* t = o["t"] | "";
  if (!p->joined && strcmp(t, "hello") != 0) return;

  if (!strcmp(t, "hello")) {
    if (p->joined || p->joining) return;
    if ((o["version"] | -1) != PROTOCOL) return p->conn->close(4000, "Your game version does not match this server");
    if (joinedCount() >= maxPlayers()) return p->conn->close(4001, "Server is full");
    p->joining = true;
    // Signed-in players prove who they are with their access token and play under their account name
    String token = o["token"] | "";
    String acct = token.length() ? verifiedName(token) : String("");
    if (p->conn->closed) return;
    p->verified = acct.length() > 0;
    cleanName(acct.length() ? acct.c_str() : (const char*)(o["name"] | ""), p->id, p->verified, p->name);
    bool localBan = false;
    String ln = p->name; ln.toLowerCase();
    for (int i = 0; i < world.nBans; i++) if (!strcmp(world.bans[i], ln.c_str())) {
        localBan = true;
        break;
      }
    if (localBan) return p->conn->close(4003, "You are banned from this server");
    if (isGloballyBanned(p->name)) return p->conn->close(4006, "You are banned from VibeBlox");
    if (p->conn->closed) return;
    restorePlayer(p);
    finishJoin(p);
    return;
  }

  if (!strcmp(t, "sleep")) {
    p->sleepSince = ((o["on"] | false) == true && isNight()) ? millis() : 0;
    return;
  }

  if (!strcmp(t, "whisper")) {
    String text = String(o["text"] | ""); text.trim(); if (text.length() > 256) text = text.substring(0, 256);
    String to = String(o["to"] | ""); to.toLowerCase();
    if (!text.length() || !to.length()) return;
    Player* target = playerByName(to);
    if (!target || target->id == p->id) return p->conn->send(String("{\"t\":\"whisperFail\",\"to\":") + jsonEsc(String(o["to"] | "").substring(0, 16)) + "}");
    target->conn->send(String("{\"t\":\"whisper\",\"from\":") + jsonEsc(p->name) + ",\"text\":" + jsonEsc(text) + "}");
    p->conn->send(String("{\"t\":\"whispered\",\"to\":") + jsonEsc(target->name) + ",\"text\":" + jsonEsc(text) + "}");
    return;
  }

  if (!strcmp(t, "title")) {
    if (p->id != hostId) return; // only the host (they are the one who may run commands)
    String text = String(o["text"] | ""); if (text.length() > 160) text = text.substring(0, 160);
    String sub = String(o["sub"] | ""); if (sub.length() > 200) sub = sub.substring(0, 200);
    String to = o["to"] | "*";
    if (!text.length() && !sub.length()) return;
    String out = String("{\"t\":\"title\",\"text\":") + jsonEsc(text) + ",\"sub\":" + jsonEsc(sub) + "}";
    if (to == "*") broadcast(out, p->id);
    else {
      String tl = to; tl.toLowerCase();
      Player* target = playerByName(tl);
      if (target && target->id != p->id) target->conn->send(out);
    }
    return;
  }

  if (!strcmp(t, "rcmd")) { // host runs /give, /clear, /effect, /xp or /tp on other players
    if (p->id != hostId) return;
    const char* cmd = o["cmd"] | "";
    if (!(strcmp(cmd, "give") == 0 || strcmp(cmd, "clear") == 0 || strcmp(cmd, "effect") == 0 ||
          strcmp(cmd, "effclear") == 0 || strcmp(cmd, "xp") == 0 || strcmp(cmd, "tp") == 0)) return;
    String args = "{}";
    JsonVariant a = o["args"];
    if (a.is<JsonObject>()) {
      serializeJson(a, args);
      if (args.length() >= 600) args = "{}";
    }
    String to = o["to"] | "*";
    String out = String("{\"t\":\"rcmd\",\"cmd\":") + jsonEsc(cmd) + ",\"args\":" + args + ",\"from\":" + jsonEsc(p->name) + "}";
    if (to == "*") broadcast(out, p->id);
    else {
      String tl = to; tl.toLowerCase();
      Player* target = playerByName(tl);
      if (target && target->id != p->id) target->conn->send(out);
    }
    return;
  }

  if (!strcmp(t, "drop")) { // a player threw an item: everyone sees it, first to pick it up gets it
    const char* nc = o["n"] | "";
    String n = nc;
    bool nOk = n.length() >= 3 && n.length() <= 16;
    if (nOk) for (size_t i = 0; i < n.length(); i++) if (!isalnum((unsigned char)n[i])) {
          nOk = false;
          break;
        }
    if (!nOk) return;
    JsonVariant itemV = o["item"], countV = o["count"];
    if (!itemV.is<int>() || !countV.is<int>()) return;
    int item = itemV, count = countV;
    if (item < 1 || item >= 4000 || count < 1 || count > 64 * 36) return;
    double x, y, z, vx, vy, vz;
    if (!isNum(o["x"], &x) || !isNum(o["y"], &y) || !isNum(o["z"], &z) ||
        !isNum(o["vx"], &vx) || !isNum(o["vy"], &vy) || !isNum(o["vz"], &vz)) return;
    if (fabs(vx) > 30 || fabs(vy) > 30 || fabs(vz) > 30) return;
    if (++p->dropsThisSecond > 40 || dropFind(n.c_str()) >= 0) return;
    if (nDrops >= 600) {
      memmove(dropsArr, dropsArr + 1, sizeof(dropsArr[0]) * 599);  // evict oldest
      nDrops = 599;
    }
    strncpy(dropsArr[nDrops].n, n.c_str(), 16); dropsArr[nDrops].n[16] = 0;
    dropsArr[nDrops].at = millis();
    nDrops++;
    String d = o["d"] | "";
    String out = String("{\"t\":\"drop\",\"n\":") + jsonEsc(n) + ",\"item\":" + item + ",\"count\":" + count +
                 ",\"x\":" + String(x, 4) + ",\"y\":" + String(y, 4) + ",\"z\":" + String(z, 4) +
                 ",\"vx\":" + String(vx, 4) + ",\"vy\":" + String(vy, 4) + ",\"vz\":" + String(vz, 4);
    if (d.length() && d.length() <= 500) out += ",\"d\":" + jsonEsc(d);
    out += "}";
    broadcast(out, p->id);
    return;
  }

  if (!strcmp(t, "pickup")) { // exactly one player gets each thrown item
    const char* n = o["n"] | "";
    int i = dropFind(n);
    if (i < 0) return p->conn->send(String("{\"t\":\"pickupNo\",\"n\":") + jsonEsc(String(n).substring(0, 16)) + "}");
    dropsArr[i] = dropsArr[--nDrops];
    p->conn->send(String("{\"t\":\"pickupOk\",\"n\":") + jsonEsc(n) + "}");
    broadcast(String("{\"t\":\"gone\",\"n\":") + jsonEsc(n) + "}", p->id);
    return;
  }

  if (!strcmp(t, "ranks")) {
    if (p->id != hostId) return;
    cleanRanks(o["ranks"]);
    worldDirty = true;
    broadcast(String("{\"t\":\"ranks\",\"ranks\":") + ranksJson() + "}", p->id);
    return;
  }

  if (!strcmp(t, "admin")) return handleAdmin(p, msg);

  if (!strcmp(t, "settings")) {
    if (p->id != hostId) return; // only the host
    JsonObject s = o["settings"];
    if (!s.isNull()) applySettingsJson(s);
    worldDirty = true;
    broadcast(String("{\"t\":\"settings\",\"settings\":{\"difficulty\":") + jsonEsc(settings.difficulty) +
              ",\"keepInventory\":" + (settings.keepInventory ? "true" : "false") +
              ",\"scripts\":" + (settings.scripts ? "true" : "false") +
              ",\"mods\":" + (settings.mods ? "true" : "false") +
              ",\"commands\":" + (settings.commands ? "true" : "false") +
              ",\"alwaysDay\":" + (settings.alwaysDay ? "true" : "false") +
              ",\"weatherCycle\":" + (settings.weatherCycle ? "true" : "false") +
              ",\"scriptPerm\":" + jsonEsc(settings.scriptPerm) + "}}");
    return;
  }

  if (!strcmp(t, "settime")) {
    if (p->id != hostId) return; // only the host
    double tm;
    if (!isNum(o["time"], &tm)) return; // ignore anything odd (never "reset to sunrise")
    timeBaseAt = millis();
    timeBaseValue = fmod(fmod(tm, 1.0) + 1.0, 1.0);
    worldDirty = true;
    broadcast(String("{\"t\":\"time\",\"time\":") + String(currentTime(), 6) + "}");
    return;
  }

  if (!strcmp(t, "weather")) {
    const char* kind = o["kind"] | "";
    bool ok = false;
    for (auto w : WEATHERS) if (!strcmp(kind, w)) ok = true;
    if (p->id != hostId || !ok) return;
    setWeather(kind);
    systemMsg(String("The host set the weather to ") + kind);
    return;
  }

  if (!strcmp(t, "kick") || !strcmp(t, "ban")) {
    if (p->id != hostId) return;
    Player* target = playerById(o["id"] | -1);
    if (!target || !target->joined || target->id == p->id) return; // never yourself
    if (!strcmp(t, "ban")) {
      String name = target->name; name.toLowerCase();
      bool have = false;
      for (int i = 0; i < world.nBans; i++) if (!strcmp(world.bans[i], name.c_str())) {
          have = true;
          break;
        }
      if (!have && world.nBans < MAX_BANS) {
        strncpy(world.bans[world.nBans++], name.c_str(), 16);
        world.bans[world.nBans - 1][16] = 0;
      }
      worldDirty = true; saveWorld();
    }
    systemMsg(String(target->name) + (!strcmp(t, "ban") ? " was banned" : " was kicked") + " by the host");
    target->conn->close(!strcmp(t, "ban") ? 4005 : 4004,
                        !strcmp(t, "ban") ? "You were banned from this server" : "You were kicked by the host");
    return;
  }

  if (!strcmp(t, "move")) {
    double x, y, z, yaw, pitch;
    if (!isNum(o["x"], &x) || !isNum(o["y"], &y) || !isNum(o["z"], &z) ||
        !isNum(o["yaw"], &yaw) || !isNum(o["pitch"], &pitch)) return;
    p->x = x; p->y = y; p->z = z; p->yaw = yaw; p->pitch = pitch;
    const char* m = o["mode"] | "survival";
    strncpy(p->mode, m, 12); p->mode[12] = 0;
    JsonVariant h = o["held"];
    p->held = (h.is<int>() && h >= 0 && h < 4000) ? (int)h : 0; // item id in hand (1400+: newer items)
    JsonVariant actV = o["act"];
    p->act = actV.is<int>() ? ((int)actV & 511) : 0; // includes newer gliding and burning bits
    p->moved = true;
    rememberPlayer(p);
    return;
  }

  if (!strcmp(t, "look")) {
    // Skin and cape names only (the game draws them); anything odd is ignored
    auto ok = [](const char* v) {
      size_t n = strlen(v);
      if (n < 1 || n > 24) return false;
      for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!islower((unsigned char)c) && !isdigit((unsigned char)c) && c != '_') return false;
      }
      return true;
    };
    const char* skin = o["skin"] | "";
    const char* cape = o["cape"] | "";
    if (!ok(skin) || !ok(cape)) return;
    String skinPng = o["custom"] | "";
    String capePng = o["capeData"] | "";
    auto isPng = [](const String & data) {
      if (data.length() > 12000 || !data.startsWith("data:image/png;base64,") || data.length() <= 22) return false;
      for (size_t i = 22; i < data.length(); i++) {
        char c = data[i];
        if (!isalnum((unsigned char)c) && c != '+' && c != '/' && c != '=') return false;
      }
      return true;
    };
    bool customSkin = !strcmp(skin, "custom");
    bool customCape = !strcmp(cape, "custom");
    if ((customSkin && !isPng(skinPng)) || (customCape && !isPng(capePng))) return;
    if (customSkin || customCape) {
      Standing st = {};
      bool entitled = p->badge.owner;
      if (p->verified) {
        if (!standingFromCache(p->name, st)) standingOf(p->name, st);
        entitled = entitled || !strcmp(st.rank, "champion") || !strcmp(st.special, "youtube");
      }
      if (p->conn->closed || !entitled) return;
    }
    char* newSkinData = customSkin ? strdup(skinPng.c_str()) : nullptr;
    char* newCapeData = customCape ? strdup(capePng.c_str()) : nullptr;
    if ((customSkin && !newSkinData) || (customCape && !newCapeData)) {
      if (newSkinData) free(newSkinData);
      if (newCapeData) free(newCapeData);
      return;
    }
    if (p->custom) free(p->custom);
    if (p->customCape) free(p->customCape);
    p->custom = newSkinData;
    p->customCape = newCapeData;
    strncpy(p->skin, skin, 24); p->skin[24] = 0;
    strncpy(p->cape, cape, 24); p->cape[24] = 0;
    String out = String("{\"t\":\"look\",\"id\":") + p->id + ",\"skin\":" + jsonEsc(skin) + ",\"cape\":" + jsonEsc(cape);
    if (p->custom) out += ",\"custom\":" + jsonEsc(p->custom);
    if (p->customCape) out += ",\"capeData\":" + jsonEsc(p->customCape);
    out += "}";
    broadcast(out, p->id);
    return;
  }

  if (!strcmp(t, "blocks")) {
    JsonVariant lv = o["list"];
    if (!lv.is<JsonArray>()) return;
    JsonArray list = lv.as<JsonArray>();
    if (list.size() > 512) return;
    String okList = "[";
    int nOk = 0;
    for (JsonVariant ev : list) {
      if (!ev.is<JsonArray>()) continue;
      JsonArray e = ev.as<JsonArray>();
      if (e.size() != 4) continue;
      long x = e[0], y = e[1], z = e[2], b = e[3];
      if (!validBlock(x, y, z, b)) continue;
      if (++p->blocksThisSecond > 600) break; // anti-griefing flood limit
      worldSetChange(x, y, z, b);
      if (nOk) okList += ",";
      okList += String("[") + x + "," + y + "," + z + "," + b + "]";
      nOk++;
    }
    if (nOk) {
      worldDirty = true;
      broadcast(String("{\"t\":\"blocks\",\"list\":") + okList + "],\"id\":" + p->id + "}", p->id);
    }
    return;
  }

  // Accept the sign message names and payload shapes used by game builds.
  if (!strncmp(t, "sign", 4)) {
    JsonVariant xv = o["x"], yv = o["y"], zv = o["z"];
    if (!isNum(xv) || !isNum(yv) || !isNum(zv)) {
      Serial.printf("[SIGN] Ignored %s message: missing numeric coordinates\r\n", t);
      return;
    }
    if (++p->blocksThisSecond > 600) return;
    JsonVariant lineValue = o["lines"];
    JsonVariant textValue = o["text"];
    char lines[4][16] = {};
    if (lineValue.is<JsonArray>() || textValue.is<JsonArray>()) {
      JsonArray input = lineValue.is<JsonArray>() ? lineValue.as<JsonArray>() : textValue.as<JsonArray>();
      for (int i = 0; i < 4 && i < (int)input.size(); i++) {
        if (!input[i].is<const char*>()) continue;
        const char* line = input[i];
        strncpy(lines[i], line, 15); lines[i][15] = 0;
      }
    } else if (textValue.is<const char*>()) {
      // Some clients send one newline-separated text field instead of four lines.
      const char* input = textValue.as<const char*>();
      int row = 0, col = 0;
      for (const char* c = input; *c && row < 4; c++) {
        if (*c == '\n' || *c == '\r') {
          if (*c == '\r' && c[1] == '\n') c++;
          row++; col = 0;
        } else if (col < 15) lines[row][col++] = *c;
      }
    } else {
      Serial.printf("[SIGN] Ignored %s message: no lines/text payload\r\n", t);
      return;
    }
    int x = (int)xv.as<long>(), y = (int)yv.as<long>(), z = (int)zv.as<long>();
    Serial.printf("[SIGN] Received %s for %d,%d,%d\r\n", t, x, y, z);
    if (!setWorldSign(x, y, z, lines)) {
      Serial.printf("[SIGN] Rejected at %d,%d,%d: sign limit reached\r\n", x, y, z);
      return;
    }
    worldDirty = true;
    saveWorld();
    if (worldDirty) {
      Serial.printf("[SIGN] ERROR: sign at %d,%d,%d was not saved to %s\r\n", x, y, z, activeWorldFile.c_str());
    } else {
      Serial.printf("[SIGN] Saved sign at %d,%d,%d (%d signs total, file=%s)\r\n", x, y, z, world.nSigns, activeWorldFile.c_str());
    }
    String lineJson = "[";
    for (int i = 0; i < 4; i++) {
      if (i) lineJson += ",";
      lineJson += jsonEsc(lines[i]);
    }
    lineJson += "]";
    broadcast(String("{\"t\":\"sign\",\"x\":") + x + ",\"y\":" + y + ",\"z\":" + z + ",\"lines\":" + lineJson + "}", p->id);
    return;
  }
  if (!strcmp(t, "ping")) { // the game shows your ping: just echo it straight back
    JsonVariant n = o["n"];
    double d;
    if (isNum(n, &d)) p->conn->send(String("{\"t\":\"pong\",\"n\":") + String(d, 2) + "}");
    return;
  }

  if (!strcmp(t, "badge")) { // refresh after a player changes undercover status
    if (!p->verified || (p->badgeAt && millis() - p->badgeAt <= 5000)) return;
    p->badgeAt = millis();
    String key = p->name; key.toLowerCase();
    for (int i = 0; i < nStandingCache; i++)
      if (standingCache[i].key == key) standingCache[i].until = 0;
    sendBadge(p);
    return;
  }

  if (!strcmp(t, "chat")) {
    String text = String(o["text"] | ""); text.trim(); if (text.length() > 256) text = text.substring(0, 256);
    if (!text.length()) return;
    String ln = p->name; ln.toLowerCase();
    if (isMuted(ln.c_str())) return p->conn->send("{\"t\":\"system\",\"text\":\"You are muted.\"}");
    if (text.equalsIgnoreCase("%espblox")) {
      uint64_t usedBytes = storageIsSD ? SD.usedBytes() : LittleFS.usedBytes();
      uint64_t totalBytes = storageIsSD ? SD.totalBytes() : LittleFS.totalBytes();
      auto sendStatus = [&](const String & line) {
        p->conn->send(String("{\"t\":\"system\",\"text\":") + jsonEsc(line) + "}");
      };
      sendStatus("ESPBlox server status");
      sendStatus(String("Model: ") + ESP.getChipModel() + " (revision " + ESP.getChipRevision() + ")");
      sendStatus(String("CPU: ") + ESP.getChipCores() + " cores at " + ESP.getCpuFreqMHz() + " MHz");
      sendStatus(String("Flash: ") + (ESP.getFlashChipSize() / (1024U * 1024U)) + " MB");
      sendStatus(String("Free heap: ") + ESP.getFreeHeap() + " bytes");
      sendStatus(String("Players: ") + joinedCount() + "/" + maxPlayers());
      sendStatus(String("Storage: ") + (storageIsSD ? "SD" : "LittleFS") + " " +
                 (uint32_t)(storageIsSD ? usedBytes / (1024ULL * 1024ULL) : usedBytes / 1024ULL) + "/" +
                 (uint32_t)(storageIsSD ? totalBytes / (1024ULL * 1024ULL) : totalBytes / 1024ULL) +
                 (storageIsSD ? " MB used/total" : " KB used/total"));
      return;
    }
    broadcast(String("{\"t\":\"chat\",\"id\":") + p->id + ",\"name\":" + jsonEsc(p->name) + ",\"text\":" + jsonEsc(text) + "}");
    return;
  }
}

// ---------------- HTTP + WebSocket upgrade ----------------
static WiFiServer server(80, 255);

static void computeWsAccept(const String& key, char out[32]) {
  const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  String input = key + magic;
  unsigned char sha1sum[20];
  mbedtls_sha1((const unsigned char*)input.c_str(), input.length(), sha1sum);
  const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String enc;
  for (int i = 0; i < 20; i += 3) {
    int b0 = sha1sum[i], b1 = i + 1 < 20 ? sha1sum[i + 1] : 0, b2 = i + 2 < 20 ? sha1sum[i + 2] : 0;
    enc += b64[b0 >> 2];
    enc += b64[((b0 & 3) << 4) | (b1 >> 4)];
    enc += (i + 1 < 20) ? b64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    enc += (i + 2 < 20) ? b64[b2 & 63] : '=';
  }
  strncpy(out, enc.c_str(), 28); out[28] = 0;
}

#if FILE_MANAGER_ENABLED
static bool fileManagerAuthorized(const String& auth) {
  if (!auth.startsWith("Basic ")) return false;
  String encoded = auth.substring(6);
  unsigned char decoded[160];
  size_t decodedLen = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decodedLen,
                            (const unsigned char*)encoded.c_str(), encoded.length()) != 0) return false;
  decoded[decodedLen] = 0;
  String expected = String("admin:") + serverConfig.adminPassword;
  return decodedLen == expected.length() && memcmp(decoded, expected.c_str(), decodedLen) == 0;
}

static void fileManagerUnauthorized(WiFiClient& sock) {
  sock.print("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"ESPBlox Admin Panel\"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  sock.stop();
}

static void fileManagerReply(WiFiClient& sock, int code, const char* status, const char* body) {
  sock.printf("HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
              code, status, (unsigned)strlen(body));
  sock.print(body);
  sock.stop();
}

static bool fileManagerName(const String& requestPath, String& name, String& fsPath, bool forUpload) {
  int q = requestPath.indexOf("name=");
  if (q < 0) return false;
  name = requestPath.substring(q + 5);
  int amp = name.indexOf('&'); if (amp >= 0) name = name.substring(0, amp);
  if (name == "world.json") fsPath = forUpload ? String(WORLD_FILE) : activeWorldFile;
  else if (name == "players.json") fsPath = PLAYER_DATA_FILE;
  else return false;
  return true;
}

static void fileManagerPage(WiFiClient& sock) {
  static const char page[] =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESPBlox Admin Panel</title><style>body{font:16px system-ui;max-width:650px;margin:2em auto;padding:0 1em;background:#101820;color:#eee}"
    "a,button{color:#8de0ff}section{background:#1b2a35;padding:1em;margin:1em 0;border-radius:8px}button{background:#17627d;border:0;padding:.7em 1em;border-radius:5px}"
    "input,select{margin:.5em 0;padding:.5em}</style></head><body><h1>ESPBlox Admin Panel</h1>"
    "<section><h2>Server mode</h2><p>Save current data and reboot in the selected mode.</p>"
    "<button onclick=\"rebootMode('safe')\">Reboot to Safe Mode</button> "
    "<button onclick=\"rebootMode('normal')\">Reboot to Normal Mode</button><p id='rebootStatus'></p></section>"
    "<section><h2>Download</h2><p><a href='/admin/download?name=world.json'>Download world.json</a></p>"
    "<p><a href='/admin/download?name=players.json'>Download players.json</a></p>"
    "</section>"
    "<section><h2>Upload</h2><p>Upload a replacement data file. The ESP32 will restart after a successful upload.</p>"
    "<select id='name'><option>world.json</option><option>players.json</option></select><br>"
    "<input id='file' type='file' accept='.json,application/json'><br><button onclick='uploadFile()'>Upload and restart</button><p id='status'></p></section>"
    "<script>async function rebootMode(mode){let s=document.getElementById('rebootStatus');s.textContent='Saving and rebooting...';try{let r=await fetch('/admin/reboot?mode='+mode,{method:'POST'});s.textContent=await r.text()}catch(e){s.textContent='Connection closed. The ESP32 may be restarting.'}}"
    "async function uploadFile(){let f=document.getElementById('file').files[0];if(!f){alert('Choose a JSON file first');return}"
    "let s=document.getElementById('status');s.textContent='Uploading...';try{let r=await fetch('/admin/upload?name='+document.getElementById('name').value,{method:'POST',body:f});"
    "s.textContent=await r.text()}catch(e){s.textContent='Connection closed. The ESP32 may be restarting.'}}</script></body></html>";
  sock.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
              (unsigned)(sizeof(page) - 1));
  sock.write((const uint8_t*)page, sizeof(page) - 1);
  sock.stop();
}

static void fileManagerDownload(WiFiClient& sock, const String& requestPath) {
  String name, fsPath;
  if (!fileManagerName(requestPath, name, fsPath, false)) {
    sock.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"); sock.stop(); return;
  }
  File f = storageFS->open(fsPath, "r");
  if (!f) {
    sock.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    sock.stop();
    return;
  }
  size_t fileSize = f.size();
  sock.printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
              name.c_str(), (unsigned)fileSize);
  uint8_t buffer[1024];
  while (f.available()) {
    size_t n = f.read(buffer, sizeof(buffer));
    if (!n || sock.write(buffer, n) != n) break;
    delay(0);
  }
  f.close(); sock.stop();
}

static void fileManagerUpload(WiFiClient& sock, const String& requestPath, size_t contentLength) {
  String name, fsPath;
  if (!fileManagerName(requestPath, name, fsPath, true)) {
    fileManagerReply(sock, 400, "Bad Request", "Only world.json or players.json are allowed"); return;
  }
  if (!contentLength || contentLength > FILE_MANAGER_MAX_UPLOAD) {
    fileManagerReply(sock, 413, "Payload Too Large", "Upload must be between 1 byte and 512 KB"); return;
  }
  String tempPath = fsPath + ".upload";
  storageFS->remove(tempPath);
  File f = storageFS->open(tempPath, "w");
  if (!f) {
    fileManagerReply(sock, 500, "Internal Server Error", "Cannot create upload file");
    return;
  }
  sock.setTimeout(15000);
  uint8_t buffer[1024];
  size_t remaining = contentLength;
  bool ok = true;
  while (remaining) {
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t got = sock.readBytes((char*)buffer, want);
    if (!got || f.write(buffer, got) != got) {
      ok = false;
      break;
    }
    remaining -= got;
    delay(0);
  }
  f.close();
  if (!ok) {
    storageFS->remove(tempPath);
    fileManagerReply(sock, 400, "Bad Request", "Upload was incomplete"); return;
  }
  storageFS->remove(fsPath);
  if (!storageFS->rename(tempPath, fsPath)) {
    storageFS->remove(tempPath);
    fileManagerReply(sock, 500, "Internal Server Error", "Could not save the file"); return;
  }
  fileManagerReply(sock, 200, "OK", "Upload saved. ESPBlox is restarting to load the file.");
  delay(400);
  ESP.restart();
}

static bool handleFileManagerHttp(WiFiClient& sock, const String& method,
                                  const String& requestPath, const String& auth,
                                  size_t contentLength) {
  if (!requestPath.startsWith("/admin")) return false;
  if (!fileManagerAuthorized(auth)) {
    fileManagerUnauthorized(sock);
    return true;
  }
  if (method == "GET" && (requestPath == "/admin" || requestPath == "/admin/")) fileManagerPage(sock);
  else if (method == "GET" && requestPath.startsWith("/admin/download")) fileManagerDownload(sock, requestPath);
  else if (method == "POST" && requestPath.startsWith("/admin/upload")) fileManagerUpload(sock, requestPath, contentLength);
  else if (method == "POST" && requestPath.startsWith("/admin/reboot")) {
    int modeAt = requestPath.indexOf("mode=");
    String mode = modeAt >= 0 ? requestPath.substring(modeAt + 5) : "";
    int amp = mode.indexOf('&');
    if (amp >= 0) mode = mode.substring(0, amp);
    if (mode != "safe" && mode != "normal") {
      fileManagerReply(sock, 400, "Bad Request", "Choose mode=safe or mode=normal");
      return true;
    }

    flushWorldChangeCache();
    saveWorld();
    savePlayerData();
    if (mode == "safe") {
      storageFS->remove(SAFE_MODE_MARKER);
      storageFS->remove(WORLD_LOAD_MARKER);
      File marker = storageFS->open(WORLD_LOAD_MARKER, "w");
      if (!marker) {
        fileManagerReply(sock, 500, "Internal Server Error", "Could not set Safe Mode marker");
        return true;
      }
      marker.print("manual safe-mode reboot");
      marker.close();
    } else {
      storageFS->remove(WORLD_LOAD_MARKER);
      storageFS->remove(SAFE_MODE_MARKER);
      storageFS->remove(SAFE_WORLD_FILE);
      storageFS->remove(String(SAFE_WORLD_FILE) + ".tmp");
    }

    fileManagerReply(sock, 200, "OK", mode == "safe" ? "Saved. Rebooting into Safe Mode." : "Saved. Rebooting into Normal Mode.");
    delay(400);
    ESP.restart();
  }
  else {
    sock.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    sock.stop();
  }
  return true;
}
#endif

static void acceptClient(WiFiClient sock) {
  DBG("Incoming TCP client from %s:%u", sock.remoteIP().toString().c_str(), (unsigned)sock.remotePort());
  // read the HTTP request headers
  String req;
  bool gotHeaders = false;
  uint32_t start = millis();
  while (millis() - start < 2000) {
    while (sock.available()) {
      req += (char)sock.read();
      if (req.endsWith("\r\n\r\n")) {
        gotHeaders = true;
        break;
      }
      if (req.length() > 8192) return;
    }
    if (gotHeaders) break;
    delay(1);
  }
  if (!gotHeaders) {
    DBG("Client request timed out before headers; bytes=%u first=%02X %02X %02X %02X", (unsigned)req.length(), req.length() > 0 ? (uint8_t)req[0] : 0, req.length() > 1 ? (uint8_t)req[1] : 0, req.length() > 2 ? (uint8_t)req[2] : 0, req.length() > 3 ? (uint8_t)req[3] : 0);
    sock.stop();
    return;
  }
  bool isUpgrade = false;
  String wsKey = "";
  String authorization = "";
  String requestLine = req.substring(0, req.indexOf("\r\n"));
  size_t contentLength = 0;
  int lineStart = 0;
  for (int i = 0; i <= req.length(); i++) {
    if (i == req.length() || (i > 0 && req[i - 1] == '\n')) {
      String line = req.substring(lineStart, i);
      line.trim();
      lineStart = i;
      int colon = line.indexOf(':');
      if (colon > 0) {
        String hn = line.substring(0, colon); hn.toLowerCase(); hn.trim();
        String hv = line.substring(colon + 1); hv.trim();
        if (hn == "sec-websocket-key") wsKey = hv;
        if (hn == "upgrade" && hv == "websocket") isUpgrade = true;
        if (hn == "authorization") authorization = hv;
        if (hn == "content-length") contentLength = (size_t)hv.toInt();
      }
    }
  }
  if (!isUpgrade || !wsKey.length()) {
    DBG("HTTP status request: %s", requestLine.c_str());
    int methodEnd = requestLine.indexOf(' ');
    int pathEnd = methodEnd >= 0 ? requestLine.indexOf(' ', methodEnd + 1) : -1;
    String method = methodEnd > 0 ? requestLine.substring(0, methodEnd) : "";
    String requestPath = pathEnd > methodEnd ? requestLine.substring(methodEnd + 1, pathEnd) : "/";
#if FILE_MANAGER_ENABLED
    if (handleFileManagerHttp(sock, method, requestPath, authorization, contentLength)) return;
#endif
    if (method == "GET" && requestPath == "/") {
      // plain HTTP status endpoint (same as the Node http handler)
      sock.print("HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\naccess-control-allow-origin: *\r\nConnection: close\r\n\r\n");
      sock.printf("VibeBlox server is running. Players online: %d/%d\r\n", joinedCount(), maxPlayers());
    } else {
      sock.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    }
    sock.stop();
    return;
  }
  if (nConns >= maxWsPlayers) {
    DBG("Rejecting WebSocket: connection limit reached (%d)", nConns);
    sock.stop();
    return;
  }
  int slot = -1;
  for (int i = 0; i < maxWsPlayers; i++) if (!players[i].conn) {
      slot = i;
      break;
    }
  if (slot < 0) {
    DBG("Rejecting WebSocket: no player slot available");
    sock.stop();
    return;
  }
  DBG("WebSocket handshake accepted");
  char accept[32];
  computeWsAccept(wsKey, accept);
  sock.print("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
  sock.printf("Sec-WebSocket-Accept: %s\r\n\r\n", accept);
  sock.setNoDelay(true);

  WsConn* c = new WsConn();
  c->sock = sock;
  c->lastSeen = millis();
  Player* p = &players[slot];
  memset(p, 0, sizeof(*p));
  p->id = nextId++;
  p->conn = c;
  strcpy(p->mode, "survival");
  c->p = p;
  conns[nConns++] = c;
  DBG("WebSocket accepted: slot=%d connections=%d free heap=%u", slot, nConns, (unsigned)ESP.getFreeHeap());
}

// ---------------- setup / loop ----------------
static uint32_t lastSave = 0, lastRateReset = 0, lastPing = 0, lastMoves = 0, lastWeatherCheck = 0, lastSleepCheck = 0, lastTimeBroadcast = 0, lastDropClean = 0;
#if PLAYER_POS_DEBUG
static void logPlayerPositions() {
  static uint32_t lastPositionLog = 0;
  uint32_t now = millis();
  if (now - lastPositionLog < 1000) return;
  lastPositionLog = now;
  int count = 0;
  for (int i = 0; i < maxWsPlayers; i++) {
    Player* p = &players[i];
    if (!p->conn || !p->joined || p->conn->closed) continue;
    char line[128];
    int len = snprintf(line, sizeof(line), "[POS %lu] id=%d name=%s x=%.2f y=%.2f z=%.2f\r\n", (unsigned long)now, p->id, p->name, p->x, p->y, p->z);
    if (len > 0 && len < (int)sizeof(line) && Serial.availableForWrite() >= len) Serial.write((const uint8_t*)line, len);
    count++;
  }
  if (!count) {
    const char* line = "[POS] no joined players\r\n";
    size_t len = strlen(line);
    if (Serial.availableForWrite() >= (int)len) Serial.write((const uint8_t*)line, len);
  }
}
#endif

static void printEspBloxStatus() {
  Serial.println("\r\n=== ESPBlox status ===");
  Serial.printf("Chip: %s, revision %d, %d cores, %u MHz\r\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                (unsigned)ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u MB at %u MHz\r\n",
                (unsigned)(ESP.getFlashChipSize() / (1024U * 1024U)),
                (unsigned)(ESP.getFlashChipSpeed() / 1000000U));
  Serial.printf("Heap: %u / %u bytes free / total\r\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize());
  Serial.printf("PSRAM: %u / %u bytes free / total\r\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
  Serial.printf("Wi-Fi: %s, IP %s, RSSI %d dBm\r\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  Serial.printf("Players: %d / %d\r\n", joinedCount(), maxPlayers());
  Serial.printf("Runtime capacity: %d player slots, %d cached block edits (RAM cache currently %d)\r\n", maxWsPlayers, maxChangeCapacity, world.nChanges);
  if (storageIsSD) {
    Serial.printf("Storage: SD card, %llu / %llu MB used / total\r\n",
                  (unsigned long long)(SD.usedBytes() / (1024ULL * 1024ULL)),
                  (unsigned long long)(SD.totalBytes() / (1024ULL * 1024ULL)));
  } else if (storageFS) {
    Serial.printf("Storage: LittleFS, %llu / %llu KB used / total\r\n",
                  (unsigned long long)(LittleFS.usedBytes() / 1024ULL),
                  (unsigned long long)(LittleFS.totalBytes() / 1024ULL));
  } else {
    Serial.println("Storage: unavailable");
  }
  Serial.println("======================\r\n");
}

static void setHostFromUart(const char* requestedName) {
  if (!isPlayerNameOk(requestedName)) {
    Serial.println("Usage: sethost <playername> (name must be 3-16 letters, numbers, or underscores)");
    return;
  }
  String lowName = requestedName;
  lowName.toLowerCase();
  Player* target = playerByName(lowName);
  if (!target) {
    Serial.printf("[UART] Cannot set host: player '%s' is not currently online.\r\n", requestedName);
    return;
  }

  Player* previousOwner = serverOwner[0] ? playerByName(String(serverOwner)) : nullptr;
  strncpy(serverOwner, target->name, 16); serverOwner[16] = 0;
  for (char* c = serverOwner; *c; c++) *c = tolower((unsigned char) * c);
  hostId = target->id;
  worldDirty = true;

  if (previousOwner && previousOwner != target) sendBadge(previousOwner);
  sendBadge(target);
  broadcast(String("{\"t\":\"host\",\"id\":") + hostId + "}");
  refreshHostRising();
  systemMsg(String(target->name) + " is now the host and server owner");
  saveWorld();
  Serial.printf("[UART] Host and owner set to %s; previous owner rank removed.\r\n", target->name);
}

static void printServerConfig() {
  Serial.println("[CONFIG] Current settings (stored in LittleFS:/settings.json):");
  Serial.printf("  port=%u\r\n  max_players=%d (0 means tier cap)\r\n  max_ws_players=%d\r\n  seed=%ld\r\n", serverConfig.port, serverConfig.maxPlayers, maxWsPlayers, serverConfig.seed);
  Serial.printf("  world_type=%s\r\n  default_mode=%s\r\n  weather_cycle=%s\r\n  experimental=%s\r\n",
                serverConfig.worldType, serverConfig.defaultMode,
                serverConfig.weatherCycle ? "true" : "false", serverConfig.experimental ? "true" : "false");
  Serial.printf("  wifi_ssid=%s\r\n  wifi_password=<hidden>\r\n  supabase_url=%s\r\n  supabase_anon_key=<hidden>\r\n  admin_password=<hidden>\r\n",
                serverConfig.wifiSsid, serverConfig.supabaseUrl);
  Serial.println("Use: config set <key> <value>");
  Serial.println("Keys: port, max_players, seed, world_type, default_mode, weather_cycle, experimental, wifi_ssid, wifi_password, supabase_url, supabase_anon_key, admin_password");
}

static bool parseConfigBool(const char* value, bool& result) {
  if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "on")) { result = true; return true; }
  if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "off")) { result = false; return true; }
  return false;
}

static bool setServerConfigValue(const char* key, const char* value) {
  char* end = nullptr;
  long number;
  if (!strcmp(key, "port")) {
    number = strtol(value, &end, 10);
    if (!*value || *end || number < 1 || number > 65535) return false;
    serverConfig.port = (uint16_t)number;
  } else if (!strcmp(key, "max_players")) {
    number = strtol(value, &end, 10);
    if (!*value || *end || number < 0 || number > maxWsPlayers) return false;
    serverConfig.maxPlayers = (int)number;
  } else if (!strcmp(key, "seed")) {
    if (!strcmp(value, "random")) serverConfig.seed = makeRandomWorldSeed();
    else {
      number = strtol(value, &end, 10);
      if (!*value || *end || number <= 0) return false;
      serverConfig.seed = number;
    }
  } else if (!strcmp(key, "world_type")) {
    if (strcmp(value, "default") && strcmp(value, "superflat")) return false;
    copyConfigText(serverConfig.worldType, sizeof(serverConfig.worldType), value);
  } else if (!strcmp(key, "default_mode")) {
    if (strcmp(value, "survival") && strcmp(value, "creative") && strcmp(value, "adventure")) return false;
    copyConfigText(serverConfig.defaultMode, sizeof(serverConfig.defaultMode), value);
  } else if (!strcmp(key, "weather_cycle") || !strcmp(key, "experimental")) {
    bool parsed;
    if (!parseConfigBool(value, parsed)) return false;
    if (!strcmp(key, "weather_cycle")) serverConfig.weatherCycle = parsed;
    else serverConfig.experimental = parsed;
  } else if (!strcmp(key, "wifi_ssid")) {
    if (!*value || strlen(value) >= sizeof(serverConfig.wifiSsid)) return false;
    copyConfigText(serverConfig.wifiSsid, sizeof(serverConfig.wifiSsid), value);
  } else if (!strcmp(key, "wifi_password")) {
    if (strlen(value) >= sizeof(serverConfig.wifiPassword)) return false;
    copyConfigText(serverConfig.wifiPassword, sizeof(serverConfig.wifiPassword), value);
  } else if (!strcmp(key, "supabase_url")) {
    if (strlen(value) >= sizeof(serverConfig.supabaseUrl)) return false;
    copyConfigText(serverConfig.supabaseUrl, sizeof(serverConfig.supabaseUrl), value);
  } else if (!strcmp(key, "supabase_anon_key")) {
    if (strlen(value) >= sizeof(serverConfig.supabaseAnonKey)) return false;
    copyConfigText(serverConfig.supabaseAnonKey, sizeof(serverConfig.supabaseAnonKey), value);
  } else if (!strcmp(key, "admin_password")) {
    if (!*value || strlen(value) >= sizeof(serverConfig.adminPassword)) return false;
    copyConfigText(serverConfig.adminPassword, sizeof(serverConfig.adminPassword), value);
  } else return false;
  return true;
}

static void handleUartConfig(char* args) {
  while (*args == ' ' || *args == '\t') args++;
  if (!*args || !strcmp(args, "show")) { printServerConfig(); return; }
  if (!strncmp(args, "set ", 4)) args += 4;
  while (*args == ' ' || *args == '\t') args++;
  char* key = args;
  char* split = key;
  while (*split && *split != ' ' && *split != '\t') split++;
  if (!*split) { printServerConfig(); return; }
  *split++ = 0;
  while (*split == ' ' || *split == '\t') split++;
  char* value = split;
  size_t valueLen = strlen(value);
  while (valueLen && (value[valueLen - 1] == ' ' || value[valueLen - 1] == '\t')) value[--valueLen] = 0;
  for (char* c = key; *c; c++) *c = tolower((unsigned char)*c);
  if (!valueLen || !setServerConfigValue(key, value)) {
    Serial.printf("[CONFIG] Invalid key/value: %s\r\n", key);
    printServerConfig();
    return;
  }
  if (!saveServerConfig()) return;
  Serial.println("[CONFIG] Updated. Rebooting to apply the new configuration...");
  Serial.flush();
  delay(700);
  ESP.restart();
}
static void pollUartCommands() {
  static char command[384];
  static size_t commandLen = 0;
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (!commandLen) continue;
      command[commandLen] = 0;
      char rawCommand[sizeof(command)];
      memcpy(rawCommand, command, commandLen + 1);
      for (size_t i = 0; i < commandLen; i++) command[i] = tolower((unsigned char)command[i]);
      if (!strcmp(command, "config")) handleUartConfig(rawCommand + 6);
      else if (!strncmp(command, "config ", 7)) handleUartConfig(rawCommand + 7);
      else if (!strcmp(command, "espblox")) printEspBloxStatus();
      else if (!strcmp(command, "halt")) {
        int online = joinedCount();
        if (online > 0) {
          systemMsg("Server is crashing in 5 seconds due to a UART halt command.");
          Serial.printf("[UART] halt requested with %d player(s) online; crashing in 5 seconds\r\n", online);
          Serial.flush();
          delay(5000);
        } else {
          Serial.println("[UART] halt requested with no players online; crashing now");
          Serial.flush();
        }
        abort();
      }
      else if (!strncmp(command, "sethost ", 8)) {
        char* name = command + 8;
        while (*name == ' ') name++;
        setHostFromUart(name);
      } else Serial.printf("Unknown command: %s (commands: config, espblox, halt, sethost <playername>)\r\n", command);
      commandLen = 0;
    } else if (commandLen < sizeof(command) - 1) {
      command[commandLen++] = ch;
    } else {
      commandLen = 0;
    }
  }
}

static void prepareWorldStartup() {
  bool crashedDuringWorldLoad = storageFS->exists(WORLD_LOAD_MARKER);
  bool previousRunWasSafe = storageFS->exists(SAFE_MODE_MARKER);

  // Safe-mode files are temporary: clear them on the next power-up/reboot.
  if (previousRunWasSafe) {
    storageFS->remove(SAFE_MODE_MARKER);
    storageFS->remove(SAFE_WORLD_FILE);
    storageFS->remove(String(SAFE_WORLD_FILE) + ".tmp");
    Serial.println("[BOOT] Removed temporary safe-mode world from the previous run");
  }

  if (crashedDuringWorldLoad) {
    storageFS->remove(WORLD_LOAD_MARKER);
    storageFS->remove(SAFE_WORLD_FILE);
    activeWorldFile = SAFE_WORLD_FILE;
    safeMode = true;
    File flag = storageFS->open(SAFE_MODE_MARKER, "w");
    if (flag) { flag.print("safe mode"); flag.close(); }
    Serial.println("[BOOT] Previous boot stopped while loading world.json; entering Safe Mode with a temporary world");
  } else {
    activeWorldFile = WORLD_FILE;
    safeMode = false;
    Serial.println("[BOOT] Normal world mode selected");
  }
}

void setup() {
  if (UART_DEBUG || PLAYER_POS_DEBUG) Serial.begin(115200);
  delay(200);
  Serial.printf("[BOOT] Reset reason=%d free heap=%u bytes\r\n", (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
  if (!allocateRuntimeCapacity()) {
    Serial.println("[BOOT] Runtime capacity allocation failed; halting startup");
    while (true) delay(1000);
  }
  DBG("Boot: chip=%s revision=%d CPU=%uMHz free heap=%u", ESP.getChipModel(), ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getFreeHeap());
  Serial.println("[CONFIG] Mounting LittleFS for settings.json...");
  littleFsReady = LittleFS.begin(true);
  if (littleFsReady) {
    Serial.println("[CONFIG] LittleFS mounted; loading settings.json");
    loadServerConfig();
  } else {
    Serial.println("[CONFIG] ERROR: LittleFS mount failed; settings will use defaults and cannot be saved");
  }
  // World and player data may use SD; settings.json always stays in LittleFS.
  Serial.println("[SD] Checking default SPI slot for an SD card...");
  if (SD.begin()) {
    storageFS = &SD;
    storageIsSD = true;
    uint8_t cardType = SD.cardType();
    const char* cardTypeName = cardType == CARD_MMC ? "MMC" :
                               cardType == CARD_SD ? "SDSC" :
                               cardType == CARD_SDHC ? "SDHC/SDXC" : "unknown";
    Serial.printf("[SD] Card detected and mounted: type=%s, size=%llu MB. Using SD for world and player data.\r\n",
                  cardTypeName, (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  } else if (littleFsReady) {
    storageFS = &LittleFS;
    Serial.println("[SD] No card detected or mount failed; using LittleFS for world and player data.");
  } else {
    Serial.println("[SD] ERROR: card unavailable and LittleFS mount failed.");
  }
  if (!storageFS) {
    while (true) delay(1000);
  }
  Serial.printf("[BOOT] Filesystem ready (%s), free heap=%u bytes\r\n", storageIsSD ? "SD" : "LittleFS", (unsigned)ESP.getFreeHeap());
  prepareWorldStartup();
  Serial.println("[BOOT] Allocating world-change RAM...");
  if (!allocateWorldChanges()) {
    Serial.printf("[BOOT] World RAM allocation failed; free heap=%u bytes\r\n", (unsigned)ESP.getFreeHeap());
    while (true) delay(1000);
  }
  Serial.printf("[BOOT] World-change RAM ready (%d entries), free heap=%u bytes\r\n", changeCapacity, (unsigned)ESP.getFreeHeap());
  timeBaseAt = millis();
  lastChangeCacheFlush = millis();
  Serial.println("[BOOT] Loading world file...");
  if (!safeMode) {
    File marker = storageFS->open(WORLD_LOAD_MARKER, "w");
    if (marker) { marker.print("loading"); marker.close(); }
    else Serial.println("[BOOT] Warning: could not write world-load crash marker");
  }
  loadWorld();
  if (!safeMode) storageFS->remove(WORLD_LOAD_MARKER);
  Serial.printf("[BOOT] World load finished (RAM cache=%d edits, %d signs), free heap=%u bytes\r\n", world.nChanges, world.nSigns, (unsigned)ESP.getFreeHeap());
  Serial.println("[BOOT] Loading player data...");
  loadPlayerData();
  Serial.printf("[BOOT] Player data load finished (%d records), free heap=%u bytes\r\n", nSavedPlayers, (unsigned)ESP.getFreeHeap());
  weatherChangeAt = millis() + nextWeatherDelay(world.weather) * 1000;

  DBG("Connecting to configured Wi-Fi network");
  Serial.println("[BOOT] Starting Wi-Fi connection...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(serverConfig.wifiSsid, serverConfig.wifiPassword);
  uint32_t lastWiFiLog = millis();
  while (WiFi.status() != WL_CONNECTED) {
    pollUartCommands(); // allow Wi-Fi settings to be corrected over UART while connecting
    delay(500);
    if (millis() - lastWiFiLog >= 10000) {
      DBG("Still waiting for Wi-Fi; status=%d", (int)WiFi.status());
      lastWiFiLog = millis();
    }
  }
  DBG("Wi-Fi connected: IP=%s RSSI=%d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  server.begin(serverConfig.port);
  DBG("Server listening on port %u", serverConfig.port);
}

void loop() {
  uint32_t now = millis();
  pollUartCommands();

  // accept new connections
  while (server.hasClient()) {
    WiFiClient c = server.available();
    if (c) acceptClient(c);
  }

  // feed data from every connection (a close inside feed only marks it closed;
  // dead WsConns are reaped at the end of the loop)
  for (int i = nConns - 1; i >= 0; i--) {
    WsConn* c = conns[i];
    if (c->closed) continue;
    if (!c->sock.connected()) {
      c->close(1000, "");
      continue;
    }
    int avail;
    while ((avail = c->sock.available()) > 0) {
      uint8_t tmp[1024];
      size_t n = avail > (int)sizeof(tmp) ? sizeof(tmp) : avail;
      size_t got = c->sock.read(tmp, n);
      if (got == 0) break;
      c->feed(tmp, got);
      if (c->closed) break;
    }
  }

  // reap closed connections (their Player slot is already released)
  int w = 0;
  for (int i = 0; i < nConns; i++) {
    WsConn* c = conns[i];
    if (c->closed) {
      if (c->buf) free(c->buf);
      delete c;
    } else {
      conns[w++] = c;
    }
  }
  nConns = w;
  logPlayerPositions();

  // Send everyone's movement 10 times a second, batched into one message
  if (now - lastMoves >= 100) {
    lastMoves = now;
    String list = "";
    for (int i = 0; i < maxWsPlayers; i++) {
      Player* p = &players[i];
      if (p->conn && p->joined && p->moved) {
        if (list.length()) list += ",";
        list += String("[") + p->id + "," + String(p->x, 2) + "," + String(p->y, 2) + "," + String(p->z, 2) +
                "," + String(p->yaw, 3) + "," + String(p->pitch, 3) + "," + jsonEsc(p->mode) + "," + p->held + "," + p->act + "]";
        p->moved = false;
      }
    }
    if (list.length()) broadcast(String("{\"t\":\"moves\",\"list\":[") + list + "]}");
  }

  // Reset rate limits every second; drop connections that went silent
  if (now - lastRateReset >= 1000) {
    lastRateReset = now;
    for (int i = 0; i < maxWsPlayers; i++) {
      Player* p = &players[i];
      if (!p->conn) continue;
      p->msgsThisSecond = 0;
      p->blocksThisSecond = 0;
      p->dropsThisSecond = 0;
      if (PLAYER_IDLE_TIMEOUT_MS > 0 && !p->conn->closed && now - p->conn->lastSeen > PLAYER_IDLE_TIMEOUT_MS) p->conn->close(4003, "Timed out");
    }
  }

  if (now - lastPing >= 20000) {
    lastPing = now;
    for (int i = 0; i < maxWsPlayers; i++)
      if (players[i].conn && !players[i].conn->closed) players[i].conn->ping();
  }

  if (now - lastSave >= 30000) {
    lastSave = now;
    saveWorld();
  }
  if (now - lastChangeCacheFlush >= WORLD_CHANGE_FLUSH_SECONDS * 1000UL) {
    lastChangeCacheFlush = now;
    flushWorldChangeCache();
  }
  static uint32_t lastPlayerSave = 0;
  if (now - lastPlayerSave >= 10000) {
    lastPlayerSave = now;
    savePlayerData();
  }

  // Weather cycle: the server decides the weather so everyone sees the same sky
  if (now - lastWeatherCheck >= 5000) {
    lastWeatherCheck = now;
    if (serverConfig.weatherCycle && settings.weatherCycle && (int32_t)(now - weatherChangeAt) >= 0) {
      if (strcmp(world.weather, "clear") != 0) setWeather("clear");
      else {
        uint32_t pick = esp_random() % 10;
        setWeather(pick < 2 ? "thunder" : pick < 4 ? "snow" : "rain");
      }
    }
  }

  // Thrown items expire like they do in the game (5 minutes)
  if (now - lastDropClean >= 30000) {
    lastDropClean = now;
    uint32_t old = now - 305000;
    int w2 = 0;
    for (int i = 0; i < nDrops; i++) if ((int32_t)(dropsArr[i].at - old) >= 0) dropsArr[w2++] = dropsArr[i];
    nDrops = w2;
  }

  // Sleeping: when at least half of the players have been in bed for 3 seconds, the night passes
  if (now - lastSleepCheck >= 500) {
    lastSleepCheck = now;
    checkSleepers();
  }

  // Keep everyone's day/night clock in step
  if (now - lastTimeBroadcast >= 30000) {
    lastTimeBroadcast = now;
    broadcast(String("{\"t\":\"time\",\"time\":") + String(currentTime(), 6) + "}");
  }

  // Save the world if WiFi drops too
  static bool wasConnected = true;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (wasConnected && !connected) {
    DBG("Wi-Fi disconnected; saving world");
    saveWorld();
  }
  else if (!wasConnected && connected) DBG("Wi-Fi reconnected: IP=%s RSSI=%d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  wasConnected = connected;

  delay(2);
}

// When at least half of the players have been in bed for 3 seconds, the night passes for everyone.
static void checkSleepers() {
  if (!isNight()) {
    for (int i = 0; i < maxWsPlayers; i++) players[i].sleepSince = 0;
    return;
  }
  int joined = 0, ready = 0;
  Player* readyOne = nullptr;
  uint32_t now = millis();
  for (int i = 0; i < maxWsPlayers; i++) {
    Player* p = &players[i];
    if (!p->conn || !p->joined) continue;
    joined++;
    if (p->sleepSince && now - p->sleepSince >= SLEEP_SECONDS * 1000) {
      ready++;
      readyOne = p;
    }
  }
  if (!joined || ready * 2 < joined) return;
  timeBaseAt = millis();
  timeBaseValue = 0.02;
  worldDirty = true;
  for (int i = 0; i < maxWsPlayers; i++) players[i].sleepSince = 0;
  broadcast(String("{\"t\":\"time\",\"time\":") + String(currentTime(), 6) + "}");
  if (strcmp(world.weather, "clear") != 0) setWeather("clear"); // like Minecraft, a night's sleep clears the storm
  systemMsg(ready == 1 ? String(readyOne->name) + " slept through the night"
            : String(ready) + " players slept through the night");
}
