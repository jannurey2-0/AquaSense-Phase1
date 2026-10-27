#include "buffer.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace {

constexpr const char *BUF_DIR = "/buf";
constexpr const char *BUF_NS  = "aqua-buf";

Preferences prefs;
uint32_t    nextSeq = 1;
bool        mounted = false;

String baseName(const char *name) {
  const char *slash = strrchr(name, '/');
  return String(slash ? slash + 1 : name);
}

bool parseSeq(const String &name, uint32_t &seq) {
  if (!name.endsWith(".json")) return false;
  const String digits = name.substring(0, name.length() - 5);
  if (digits.length() == 0) return false;
  for (size_t i = 0; i < digits.length(); i++) {
    if (!isDigit(digits[i])) return false;
  }
  seq = strtoul(digits.c_str(), nullptr, 10);
  return true;
}

bool findOldestPath(String &path) {
  File root = LittleFS.open(BUF_DIR);
  if (!root) return false;

  uint32_t best  = UINT32_MAX;
  bool     found = false;
  File     entry = root.openNextFile();
  while (entry) {
    uint32_t seq = 0;
    if (parseSeq(baseName(entry.name()), seq) && seq < best) {
      best  = seq;
      found = true;
    }
    entry = root.openNextFile();
  }
  root.close();

  if (!found) return false;
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%s/%07lu.json", BUF_DIR, (unsigned long)best);
  path = tmp;
  return true;
}

}  // namespace

namespace buffer {

void begin() {
  mounted = LittleFS.begin(true);
  if (!mounted) {
    Serial.println("[buf] LittleFS mount failed");
    return;
  }
  if (!LittleFS.exists(BUF_DIR)) LittleFS.mkdir(BUF_DIR);

  prefs.begin(BUF_NS, false);
  nextSeq = prefs.getUInt("seq", 1);
  prefs.end();
  if (nextSeq == 0) nextSeq = 1;

  Serial.printf("[buf] %u record(s) pending\n", (unsigned)count());
}

size_t count() {
  if (!mounted) return 0;

  File root = LittleFS.open(BUF_DIR);
  if (!root) return 0;

  size_t n     = 0;
  File   entry = root.openNextFile();
  while (entry) {
    uint32_t seq = 0;
    if (parseSeq(baseName(entry.name()), seq)) n++;
    entry = root.openNextFile();
  }
  root.close();
  return n;
}

bool push(const String &json) {
  if (!mounted || json.length() == 0) return false;

  if (count() >= BUFFER_MAX_RECORDS) {
    Serial.println("[buf] full - dropping oldest record");
    removeOldest();
  }

  char path[32];
  snprintf(path, sizeof(path), "%s/%07lu.json", BUF_DIR, (unsigned long)nextSeq);

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return false;
  const size_t written = f.print(json);
  f.close();

  if (written == 0) {
    LittleFS.remove(path);
    return false;
  }

  nextSeq++;
  prefs.begin(BUF_NS, false);
  prefs.putUInt("seq", nextSeq);
  prefs.end();
  return true;
}

bool peekOldest(String &out) {
  if (!mounted) return false;

  String path;
  if (!findOldestPath(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out = f.readString();
  f.close();
  return out.length() > 0;
}

bool removeOldest() {
  if (!mounted) return false;

  String path;
  if (!findOldestPath(path)) return false;
  return LittleFS.remove(path);
}

void clear() {
  while (count() > 0) {
    if (!removeOldest()) break;
  }
}

}  // namespace buffer
