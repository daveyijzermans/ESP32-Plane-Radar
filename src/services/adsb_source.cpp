#include "services/adsb_source.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

namespace services::adsb_source {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyUrl[] = "src_url";

char s_url[kMaxUrlLen + 1] = "";

/** Accept http://, https:// or mqtt:// followed by at least one host character. */
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
  } else if (strncmp(url, "mqtt://", 7) == 0) {
    host = url + 7;
  } else {
    return false;
  }
  return *host != '\0' && *host != '/';
}

/** Where an mqtt:// URL's password starts and ends ("mqtt://user:PASS@host..."); false when it has none. */
bool passwordSpan(const char* url, size_t* start, size_t* end) {
  if (strncmp(url, "mqtt://", 7) != 0) {
    return false;
  }
  const char* p = url + 7;
  const char* slash = strchr(p, '/');
  const char* at = nullptr;
  for (const char* q = p; *q && (slash == nullptr || q < slash); ++q) {
    if (*q == '@') {
      at = q;
    }
  }
  if (at == nullptr) {
    return false;
  }
  const char* colon = static_cast<const char*>(memchr(p, ':', at - p));
  if (colon == nullptr) {
    return false;
  }
  *start = static_cast<size_t>(colon + 1 - url);
  *end = static_cast<size_t>(at - url);
  return true;
}

/** Copy url with its password span replaced by with; false when it does not fit. */
bool replacePassword(const char* url, size_t start, size_t end, const char* with, char* out, size_t out_len) {
  const size_t total = start + strlen(with) + strlen(url + end);
  if (total >= out_len) {
    return false;
  }
  memcpy(out, url, start);
  strcpy(out + start, with);
  strcat(out, url + end);
  return true;
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

bool isMqtt() { return strncmp(s_url, "mqtt://", 7) == 0; }

void displayUrl(char* out, size_t out_len) {
  size_t start = 0, end = 0;
  if (!passwordSpan(s_url, &start, &end) || !replacePassword(s_url, start, end, kPasswordMask, out, out_len)) {
    strncpy(out, s_url, out_len - 1);
    out[out_len - 1] = '\0';
  }
}

bool saveFromString(const char* url_str) {
  char buf[kMaxUrlLen + 1] = "";
  trimmedCopy(url_str, buf, sizeof(buf));

  // The portal shows the password masked; a save that keeps the mask keeps the stored password.
  size_t start = 0, end = 0, old_start = 0, old_end = 0;
  if (passwordSpan(buf, &start, &end) && end - start == strlen(kPasswordMask) &&
      strncmp(buf + start, kPasswordMask, end - start) == 0 && passwordSpan(s_url, &old_start, &old_end)) {
    char password[kMaxUrlLen + 1];
    memcpy(password, s_url + old_start, old_end - old_start);
    password[old_end - old_start] = '\0';
    char merged[kMaxUrlLen + 1];
    if (!replacePassword(buf, start, end, password, merged, sizeof(merged))) {
      return false;
    }
    memcpy(buf, merged, sizeof(buf));
  }

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
  char shown[kMaxUrlLen + 1];
  displayUrl(shown, sizeof(shown));
  Serial.printf("ADS-B source saved: %s\n", shown);
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
