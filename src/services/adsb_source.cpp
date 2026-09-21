#include "services/adsb_source.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

namespace services::adsb_source {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyUrl[] = "src_url";

char s_url[kMaxUrlLen + 1] = "";

/** Accept http:// or https:// followed by at least one host character. */
bool validUrl(const char* url) {
  if (url == nullptr) {
    return false;
  }
  const size_t len = strnlen(url, kMaxUrlLen + 1);
  if (len == 0 || len > kMaxUrlLen) {
    return false;
  }
  const char* host = nullptr;
  if (strncmp(url, "http://", 7) == 0) {
    host = url + 7;
  } else if (strncmp(url, "https://", 8) == 0) {
    host = url + 8;
  } else {
    return false;
  }
  return *host != '\0' && *host != '/';
}

void trimmedCopy(const char* in, char* out, size_t out_len) {
  out[0] = '\0';
  if (in == nullptr || out_len == 0) {
    return;
  }
  while (*in == ' ' || *in == '\t') {
    ++in;
  }
  size_t n = strnlen(in, out_len - 1);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) {
    --n;
  }
  memcpy(out, in, n);
  out[n] = '\0';
}

}  // namespace

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  if (prefs.isKey(kKeyUrl)) {
    char buf[kMaxUrlLen + 1] = "";
    prefs.getString(kKeyUrl, buf, sizeof(buf));
    if (validUrl(buf)) {
      memcpy(s_url, buf, sizeof(buf));
    }
  }
  prefs.end();
}

const char* url() { return s_url; }

bool saveFromString(const char* url_str) {
  char buf[kMaxUrlLen + 1] = "";
  trimmedCopy(url_str, buf, sizeof(buf));

  if (buf[0] == '\0') {
    clear();
    Serial.println("ADS-B source cleared — radar will not fetch");
    return true;
  }
  if (!validUrl(buf)) {
    return false;
  }

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kKeyUrl, buf);
    prefs.end();
  }
  memcpy(s_url, buf, sizeof(buf));
  Serial.printf("ADS-B source saved: %s\n", s_url);
  return true;
}

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.remove(kKeyUrl);
    prefs.end();
  }
  s_url[0] = '\0';
}

}  // namespace services::adsb_source
