#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "services/adsb_source.h"

namespace services::adsb {

namespace {

constexpr float kEarthRadiusKm = 6371.0f;
/** Drop positions older than this; local feeds keep faded targets listed. */
constexpr float kMaxPositionAgeSec = 60.0f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

/** Failed polls tolerated before the list is dropped, so a dead feed goes
 *  blank instead of leaving stale targets on screen. */
constexpr unsigned kMaxFailedPolls = 3;
unsigned s_failed_polls = 0;

void noteFetchFailure() {
  if (++s_failed_polls >= kMaxFailedPolls && s_aircraft_count > 0) {
    s_aircraft_count = 0;
    Serial.println("adsb: feed stale, clearing targets");
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

/**
 * Feeds the JSON parser straight from the socket, so a whole aircraft.json
 * never has to fit in RAM, and keeps the portal alive while blocked on data.
 */
class PollingStreamReader {
 public:
  explicit PollingStreamReader(WiFiClient* stream)
      : stream_(stream), deadline_(millis() + kRequestTimeoutMs) {}

  int read() {
    if (!waitForData()) {
      return -1;
    }
    return stream_->read();
  }

  size_t readBytes(char* buffer, size_t length) {
    if (!waitForData()) {
      return 0;
    }
    return stream_->readBytes(reinterpret_cast<uint8_t*>(buffer), length);
  }

 private:
  bool waitForData() {
    while (stream_->available() <= 0) {
      if (!stream_->connected() || millis() > deadline_) {
        return false;
      }
      pollNetwork();
      delay(1);
    }
    return true;
  }

  WiFiClient* stream_;
  unsigned long deadline_;
};

float greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
  const double to_rad = PI / 180.0;
  const double dlat = (lat2 - lat1) * to_rad;
  const double dlon = (lon2 - lon1) * to_rad;
  const double mid_lat = (lat1 + lat2) * 0.5 * to_rad;
  const double x = dlon * cos(mid_lat);
  return static_cast<float>(sqrt(dlat * dlat + x * x) * kEarthRadiusKm);
}

/** Keys kept while parsing, so a busy feed cannot exhaust RAM. */
void buildParseFilter(JsonDocument& filter) {
  JsonObject plane = filter["aircraft"].add<JsonObject>();
  plane["lat"] = true;
  plane["lon"] = true;
  plane["track"] = true;
  plane["true_heading"] = true;
  plane["mag_heading"] = true;
  plane["dir"] = true;
  plane["gs"] = true;
  plane["tas"] = true;
  plane["ias"] = true;
  plane["alt_baro"] = true;
  plane["alt_geom"] = true;
  plane["flight"] = true;
  plane["hex"] = true;
  plane["t"] = true;
  plane["seen_pos"] = true;
  filter["ac"] = filter["aircraft"];
}

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

bool positionIsStale(const JsonObject& plane) {
  float seen = 0.0f;
  if (!readJsonFloat(plane, "seen_pos", &seen)) {
    return false;
  }
  return seen > kMaxPositionAgeSec;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const char* url = services::adsb_source::url();
  if (url[0] == '\0') {
    s_aircraft_count = 0;
    Serial.println("adsb: no source URL configured");
    return false;
  }
  noteFetchFailure();  // cleared again once this poll succeeds

  const bool secure = strncmp(url, "https://", 8) == 0;
  WiFiClient plain_client;
  WiFiClientSecure tls_client;
  if (secure) {
    tls_client.setInsecure();
  }
  WiFiClient& client = secure ? static_cast<WiFiClient&>(tls_client) : plain_client;

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.useHTTP10(true);
  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    return false;
  }

  JsonDocument filter;
  buildParseFilter(filter);

  JsonDocument doc;
  PollingStreamReader reader(stream);
  const DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }
  s_failed_polls = 0;

  // "aircraft" is dump1090/readsb aircraft.json; "ac" is the adsb.fi/re-api shape.
  JsonArray ac = doc["aircraft"].as<JsonArray>();
  if (ac.isNull()) {
    ac = doc["ac"].as<JsonArray>();
  }
  if (ac.isNull()) {
    s_aircraft_count = 0;
    Serial.println("adsb: no aircraft array in response");
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }
    if (positionIsStale(plane)) {
      continue;
    }
    // Local feeds are unfiltered: keep only what the radar can place.
    if (greatCircleKm(center_lat, center_lon, plane["lat"].as<double>(),
                      plane["lon"].as<double>()) > fetch_radius_km) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

}  // namespace services::adsb
