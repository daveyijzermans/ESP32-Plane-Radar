#pragma once

#include <cstddef>

namespace services::adsb_source {

/** Max stored URL length (excluding NUL). */
constexpr size_t kMaxUrlLen = 127;

/** Load the saved aircraft.json URL from NVS. Call once before WiFi setup. */
void init();

/** Configured source URL, or "" when none is set (then nothing is fetched). */
const char* url();

/** Parse a portal string, validate scheme/host, persist to NVS. */
bool saveFromString(const char* url_str);

/** Clear the stored URL (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::adsb_source
