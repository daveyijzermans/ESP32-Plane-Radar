#pragma once

#include <cstddef>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/**
 * Fetch aircraft within fetch_radius_km of center_lat/lon from the source in
 * services::adsb_source: an http(s) URL (dump1090/readsb aircraft.json, or an "ac"-shaped API), polled, or an
 * mqtt:// source whose latest message (the same shape) is taken. True when there is a new list to draw.
 * Filtering by radius happens here: local feeds return everything they see.
 */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/** Keep an mqtt:// source's connection and take its messages; call every loop. A no-op for http(s). */
void loop();

}  // namespace services::adsb
