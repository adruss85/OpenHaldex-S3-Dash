// =====================================================================
// logbook.cpp - see logbook.h for what this is and why it is split in two.
// =====================================================================
#include "app/logbook.h"

#include <LittleFS.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <math.h>
#include <string.h>

#include "net/datafeed.h"

namespace logbook {
namespace {

// The partition is declared "littlefs" in partitions_4mb_ota.csv and mounted at
// the Arduino default. Paths stay *relative to the mount point*: the VFS
// prepends /littlefs itself, so "/drive.csv" is /littlefs/drive.csv on disk and
// "/littlefs/drive.csv" would be /littlefs/littlefs/drive.csv (measured: the
// first session refused to open, and the mkdir it tried left a stray directory).
constexpr const char *kLabel = "littlefs";
constexpr const char *kMount = "/littlefs";
constexpr const char *kPath = "/drive.csv";
// What the console and the docs call it, so a person reading either sees the
// path they would type into a shell on the filesystem.
constexpr const char *kShownPath = "/littlefs/drive.csv";

// 5 Hz. Fast enough to see a boost spike or a mode step between two frames,
// and 200 rows in the ring is still 40 s of history at ~15 KB of PSRAM.
constexpr uint32_t kSampleMs = 200;
constexpr uint16_t kRingRows = 200;
constexpr uint8_t kRowCap = 96;

// Batches, not rows: one 1 KB program is one cache-off window instead of five.
constexpr size_t kFlushAt = 900;
constexpr size_t kBufCap = 1024;

// 100 KB of a 128 KB partition. The rest is LittleFS metadata and the slack a
// block-level filesystem needs to append without compacting constantly.
constexpr size_t kMaxBytes = 100 * 1024;
constexpr uint32_t kMaxSessionMs = 10ul * 60ul * 1000ul;

const char kHeader[] =
    "ms,rpm,speed_kmh,throttle_pct,boost_mbar,coolant_c,oil_c,fuel_l,"
    "mode,lock_tgt,lock_act,steer_deg,brake_can,hand_can,cruise,src\n";

SemaphoreHandle_t g_fs = nullptr;
File g_file;
bool g_mounted = false;
bool g_rec = false;

char g_buf[kBufCap];
size_t g_bufLen = 0;
size_t g_bytes = 0;
uint32_t g_startedMs = 0;
uint32_t g_dropped = 0;
uint32_t g_lastSampleMs = 0;

char *g_ring = nullptr;      // kRingRows * kRowCap in PSRAM
uint16_t g_ringHead = 0;     // next slot to write
uint16_t g_ringCount = 0;    // how many slots hold a row

WebServer *g_srv = nullptr;
char g_line[64] = "log: idle";

void stamp() {
  if (!g_mounted) {
    snprintf(g_line, sizeof g_line, "log: no filesystem");
    return;
  }
  if (!g_rec) {
    snprintf(g_line, sizeof g_line, "log: idle %u B", (unsigned)g_bytes);
    return;
  }
  const uint32_t s = (millis() - g_startedMs) / 1000;
  snprintf(g_line, sizeof g_line, "log: REC %u%% %u:%02u",
           (unsigned)((g_bytes * 100) / kMaxBytes), (unsigned)(s / 60), (unsigned)(s % 60));
}

// A number or nothing. NaN is a real state here (the car has no such sensor)
// and an empty field is how a CSV says "unknown" without inventing a zero.
int addF(char *dst, size_t cap, int o, float v, int dec) {
  if (o >= (int)cap - 2) return (int)cap - 1;
  dst[o++] = ',';
  if (isnan(v)) return o;
  const int w = snprintf(dst + o, cap - (size_t)o, "%.*f", dec, (double)v);
  if (w <= 0) return o;
  return (o + w < (int)cap) ? o + w : (int)cap - 1;
}

// An integer, where negative means "not heard" rather than a value.
int addI(char *dst, size_t cap, int o, int v) {
  if (o >= (int)cap - 2) return (int)cap - 1;
  dst[o++] = ',';
  if (v < 0) return o;
  const int w = snprintf(dst + o, cap - (size_t)o, "%d", v);
  if (w <= 0) return o;
  return (o + w < (int)cap) ? o + w : (int)cap - 1;
}

int buildRow(char *out, size_t cap) {
  const telemetry::Live v = feed::live();
  const telemetry::Dash d = feed::dash();

  int o = snprintf(out, cap, "%lu", (unsigned long)millis());
  o = addF(out, cap, o, v.rpm, 0);
  o = addF(out, cap, o, v.speed, 0);
  o = addF(out, cap, o, v.throttle, 1);
  o = addF(out, cap, o, v.boost, 0);
  o = addF(out, cap, o, v.coolant, 0);
  o = addF(out, cap, o, v.oil, 0);
  o = addF(out, cap, o, v.fuel, 1);
  o = addI(out, cap, o, d.mode);
  o = addI(out, cap, o, d.lockTarget);
  o = addI(out, cap, o, d.lockActual);
  o = addF(out, cap, o, d.steeringAngle, 1);
  o = addI(out, cap, o, d.brakeFromCAN);
  o = addI(out, cap, o, d.handbrakeFromCAN);
  o = addI(out, cap, o, d.cruise);

  // Which transport the fast values above came from, so a trace that steps or
  // goes blank can be read for what it is instead of looking like a fault.
  int src = 0;
  if (feed::source() == feed::Source::EspNow) src = 1;
  else if (feed::source() == feed::Source::Http) src = 2;
  o = addI(out, cap, o, src);

  if (o >= (int)cap - 1) o = (int)cap - 2;
  out[o++] = '\n';
  out[o] = '\0';
  return o;
}

void ringPush(const char *row) {
  if (!g_ring) return;
  char *slot = g_ring + (size_t)g_ringHead * kRowCap;
  strncpy(slot, row, kRowCap - 1);
  slot[kRowCap - 1] = '\0';
  g_ringHead = (uint16_t)((g_ringHead + 1) % kRingRows);
  if (g_ringCount < kRingRows) g_ringCount++;
}

// Oldest first, so a dump reads in time order.
const char *ringAt(uint16_t i) {
  if (!g_ring || i >= g_ringCount) return nullptr;
  const uint16_t start = (uint16_t)((g_ringHead + kRingRows - g_ringCount) % kRingRows);
  return g_ring + (size_t)((start + i) % kRingRows) * kRowCap;
}

// Append the buffer to the open file. Never waits: a download holding the
// filesystem delays this by one sample rather than stalling the panel.
void flush(bool wait) {
  if (!g_rec || g_bufLen == 0) return;
  if (!g_fs) return;
  const TickType_t give = wait ? pdMS_TO_TICKS(1000) : 0;
  if (xSemaphoreTake(g_fs, give) != pdTRUE) return;
  const size_t want = g_bufLen;
  const size_t wrote = g_file.write((const uint8_t *)g_buf, want);
  g_file.flush();
  xSemaphoreGive(g_fs);
  g_bytes += wrote;
  g_bufLen = 0;
  if (wrote < want) {
    // A short write means the partition is full. Keep what was written and
    // close cleanly instead of retrying the tail forever.
    Serial.printf("[log] short write (%u of %u B) - stopping\n", (unsigned)wrote, (unsigned)want);
    stop();
  }
}

void sample() {
  char row[kRowCap];
  const int n = buildRow(row, sizeof row);
  if (n <= 0) return;
  ringPush(row);
  if (!g_rec) return;

  if (g_bufLen + (size_t)n >= sizeof g_buf) flush(false);
  if (g_bufLen + (size_t)n < sizeof g_buf) {
    memcpy(g_buf + g_bufLen, row, (size_t)n);
    g_bufLen += (size_t)n;
  } else {
    g_dropped++;  // the filesystem was busy and the batch is full
  }
  if (g_bufLen >= kFlushAt) flush(false);
}

// Is the data partition still erased? This is the one case where formatting is
// safe, and it is the state a new board is in: the CrowPanel's factory table
// left this 128 KB region blank, so the first mount failed with
// LFS_ERR_CORRUPT ("Corrupted dir pair at {0x0,0x1}") on a chip full of 0xFF.
// Blocks 0 and 1 carry LittleFS's metadata pair, so reading those is enough to
// tell a blank region from a damaged one.
bool partitionBlank() {
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kLabel);
  if (!p) return false;
  uint8_t buf[256];
  for (size_t off = 0; off < 2 * 4096; off += sizeof buf) {
    if (esp_partition_read(p, off, buf, sizeof buf) != ESP_OK) return false;
    for (size_t i = 0; i < sizeof buf; i++) {
      if (buf[i] != 0xFF) return false;
    }
  }
  return true;
}

void ensureRing() {
  g_ringHead = g_ringCount = 0;
  if (g_ring) return;
  g_ring = (char *)heap_caps_malloc((size_t)kRingRows * kRowCap,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_ring) Serial.println("[log] no PSRAM for the ring - sessions only, no pre-trigger");
}

// ---- HTTP ------------------------------------------------------------
void redirectRoot() {
  g_srv->sendHeader("Location", "/");
  g_srv->send(303, "text/plain", "");
}

void handleStart() {
  start();
  redirectRoot();
}

void handleStop() {
  stop();
  redirectRoot();
}

void handleDelete() {
  if (g_rec) {
    g_srv->send(409, "text/plain", "stop the log first");
    return;
  }
  if (g_fs && xSemaphoreTake(g_fs, pdMS_TO_TICKS(2000)) == pdTRUE) {
    LittleFS.remove(kPath);
    xSemaphoreGive(g_fs);
    g_bytes = 0;
    Serial.println("[log] drive.csv deleted");
    redirectRoot();
    return;
  }
  g_srv->send(503, "text/plain", "filesystem busy");
}

void handleFormat() {
  if (formatData()) {
    redirectRoot();
    return;
  }
  g_srv->send(500, "text/plain", "format failed - see the console");
}

void handleCsv() {
  if (!g_mounted || !g_fs) {
    g_srv->send(404, "text/plain", "no filesystem");
    return;
  }
  if (xSemaphoreTake(g_fs, pdMS_TO_TICKS(2000)) != pdTRUE) {
    g_srv->send(503, "text/plain", "filesystem busy");
    return;
  }
  File f = LittleFS.open(kPath, "r");
  if (!f) {
    xSemaphoreGive(g_fs);
    g_srv->send(404, "text/plain", "no log yet");
    return;
  }
  const size_t n = f.size();
  g_srv->sendHeader("Content-Disposition", "attachment; filename=\"drive.csv\"");
  g_srv->send(200, "text/csv", f, n);
  f.close();
  xSemaphoreGive(g_fs);
  Serial.printf("[log] drive.csv downloaded (%u B)\n", (unsigned)n);
}

}  // namespace

// ------------------------------------------------------------ public API
void begin() {
  g_fs = xSemaphoreCreateMutex();
  if (!g_fs) {
    Serial.println("[log] no mutex - logging disabled");
    return;
  }

  if (!LittleFS.begin(false, kMount, 10, kLabel)) {
    // Default is still to leave it alone: a partition that will not mount may
    // hold the log somebody is trying to get off it. The exception is a blank
    // partition - nothing to lose, and a new board's is blank.
    if (!partitionBlank()) {
      Serial.println("[log] littlefs will not mount and the partition is not blank - left as");
      Serial.println("      it is; 'F' on the console or the update page formats it (erases it)");
      return;
    }
    Serial.println("[log] data partition is blank - formatting it once (nothing to lose)");
    if (!LittleFS.format() || !LittleFS.begin(false, kMount, 10, kLabel)) {
      Serial.println("[log] format did not produce a mountable filesystem - logging disabled");
      return;
    }
    Serial.println("[log] formatted and mounted");
  }
  g_mounted = true;
  ensureRing();

  size_t stored = 0;
  if (xSemaphoreTake(g_fs, pdMS_TO_TICKS(1000)) == pdTRUE) {
    File f = LittleFS.open(kPath, "r");
    if (f) {
      stored = f.size();
      f.close();
    }
    xSemaphoreGive(g_fs);
  }
  g_bytes = stored;
  stamp();
  Serial.printf("[log] %s | part %u/%u B used | ring %s\n", g_line, (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes(), g_ring ? "in PSRAM" : "OFF");
}

bool formatData() {
  if (!g_fs) return false;
  if (g_rec) stop();  // the open File would survive the reformat otherwise
  if (xSemaphoreTake(g_fs, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.println("[log] cannot format: filesystem busy");
    return false;
  }
  // begin() succeeds when the partition is already mounted and only remembers
  // the label, so unmount first: esp_littlefs_format refuses a mounted volume.
  if (LittleFS.begin(false, kMount, 10, kLabel)) LittleFS.end();
  const bool formatted = LittleFS.format();
  const bool mounted = formatted && LittleFS.begin(false, kMount, 10, kLabel);
  xSemaphoreGive(g_fs);

  g_mounted = mounted;
  g_bytes = 0;
  if (mounted) ensureRing();
  stamp();
  Serial.printf("[log] format: %s | partition %u/%u B used\n", mounted ? "mounted" : "FAILED",
                mounted ? (unsigned)LittleFS.usedBytes() : 0u,
                mounted ? (unsigned)LittleFS.totalBytes() : 0u);
  return mounted;
}

bool ready() { return g_mounted; }

void start() {
  if (g_rec) return;
  if (!g_mounted || !g_fs) {
    Serial.println("[log] cannot record: no filesystem");
    return;
  }

  if (xSemaphoreTake(g_fs, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("[log] cannot record: filesystem busy");
    return;
  }
  g_file = LittleFS.open(kPath, "w");
  if (!g_file) {
    xSemaphoreGive(g_fs);
    Serial.println("[log] cannot open drive.csv for writing");
    return;
  }
  size_t wrote = g_file.write((const uint8_t *)kHeader, strlen(kHeader));

  // The pre-trigger half: the ring is already in time order, so the file
  // starts with the seconds before the button was pressed.
  if (g_ring) {
    for (uint16_t i = 0; i < g_ringCount; i++) {
      const char *row = ringAt(i);
      if (row) wrote += g_file.write((const uint8_t *)row, strlen(row));
    }
  }
  g_file.flush();
  xSemaphoreGive(g_fs);

  g_rec = true;
  g_bytes = wrote;
  g_bufLen = 0;
  g_dropped = 0;
  g_startedMs = millis();
  stamp();
  Serial.printf("[log] %s -> %s (%u rows of pre-trigger)\n", g_line, kShownPath,
                (unsigned)g_ringCount);
}

void stop() {
  if (!g_rec) return;
  flush(true);
  if (g_fs && xSemaphoreTake(g_fs, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (g_file) {
      g_file.flush();
      g_file.close();
    }
    xSemaphoreGive(g_fs);
  } else {
    // A download held the filesystem for longer than we were willing to wait.
    // The writer is left open; the next start() replaces the File and closes it.
    Serial.println("[log] could not close drive.csv: filesystem busy");
  }
  g_rec = false;
  stamp();
  Serial.printf("[log] %s | %u B, %u row(s) lost to a busy filesystem\n", g_line, (unsigned)g_bytes,
                (unsigned)g_dropped);
}

void toggle() {
  if (g_rec) stop();
  else start();
}

bool recording() { return g_rec; }

size_t fileBytes() { return g_bytes; }

const char *stateLine() {
  if (g_rec) stamp();
  return g_line;
}

void printStatus() {
  stamp();
  Serial.printf("[log] %s | file %u B of %u | ring %u/%u rows | dropped %u\n", g_line,
                (unsigned)g_bytes, (unsigned)kMaxBytes, (unsigned)g_ringCount, (unsigned)kRingRows,
                (unsigned)g_dropped);
}

void attach(WebServer &server) {
  g_srv = &server;
  server.on("/log.csv", HTTP_GET, handleCsv);
  server.on("/log/start", HTTP_POST, handleStart);
  server.on("/log/stop", HTTP_POST, handleStop);
  server.on("/log/delete", HTTP_POST, handleDelete);
  server.on("/log/format", HTTP_POST, handleFormat);
}

String logHtml() {
  String s = "<h2>Drive log</h2><p>";
  if (g_rec) {
    s += "Recording. ";
    s += stateLine();
  } else if (g_bytes) {
    s += "Not recording. ";
    s += String((unsigned)(g_bytes / 1024));
    s += " KB stored.";
  } else {
    s += "Not recording. Nothing stored yet.";
  }
  s += "</p>";

  if (!g_mounted) {
    s += "<p class='warn'>The data partition holds no readable filesystem, so logging is "
         "off. It is not formatted automatically when it may hold a log to rescue.</p>";
    s += "<form method='post' action='/log/format' "
         "onsubmit='return confirm(&quot;Format the data partition? Anything in it is erased.&quot;)'>"
         "<button>Format partition</button></form>";
    return s;
  }

  if (g_bytes && !g_rec) {
    s += "<p><a href='/log.csv' style='color:#7fd1ff'>Download drive.csv</a> - "
         "a value the car does not have is an empty field, never a zero.</p>";
  }
  s += "<form method='post' action='/log/start' style='display:inline'>"
       "<button>Start log</button></form> ";
  if (g_rec) {
    s += "<form method='post' action='/log/stop' style='display:inline'>"
         "<button>Stop log</button></form>";
  } else if (g_bytes) {
    s += "<form method='post' action='/log/delete' style='display:inline'>"
         "<button>Delete</button></form>";
  }
  s += "<p>Stops by itself at 100 KB (about eight minutes at 5 Hz) or ten minutes.</p>";
  return s;
}

void tick() {
  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastSampleMs) >= kSampleMs) {
    g_lastSampleMs = now;
    sample();
  }
  if (!g_rec) return;
  if (g_bytes >= kMaxBytes) {
    Serial.printf("[log] %u B is the limit\n", (unsigned)g_bytes);
    stop();
  } else if ((uint32_t)(now - g_startedMs) >= kMaxSessionMs) {
    Serial.println("[log] ten minutes is the limit");
    stop();
  }
}

}  // namespace logbook
