#pragma once

#include <string>

namespace ma::node {

// Device and network helpers shared by the ONVIF service and WS-Discovery.
// All values are read lazily; serial number / UUID are cached on first use
// (they never change at runtime), the local IP is re-resolved per call so
// DHCP address changes are picked up.

namespace onvif {

// Serial number from the U-Boot environment ("fw_printenv -n sn"), same
// source the supervisor uses. Falls back to the hostname when unavailable.
const std::string& serialNumber();

// Firmware version from the CMake-generated version.h.
const std::string& firmwareVersion();

// Fixed marketing model string.
const std::string& model();

// Stable urn:uuid derived from the serial number (survives reboots).
const std::string& endpointUUID();

// First non-loopback IPv4 address of the device, empty when none is up.
std::string localIP();

// Kernel hostname.
std::string hostname();

// Current UTC time broken down, plus local offset in seconds east of UTC.
void systemTime(int& year, int& mon, int& day, int& hour, int& min, int& sec, long& utcOffset);

// base64 of a raw byte buffer.
std::string base64Encode(const unsigned char* data, size_t len);

// Decodes base64 (whitespace tolerated, missing padding accepted).
std::string base64Decode(const std::string& in);

// SHA-1 digest of a buffer (20 raw bytes appended to out).
void sha1(const std::string& in, unsigned char out[20]);

// Extract the text between "<tag ...>text</tag>" (first occurrence,
// namespace prefixes on the tag are ignored). Empty when not found.
std::string xmlText(const std::string& xml, const std::string& tag);

// True when the element `tag` appears anywhere in the body.
bool xmlHas(const std::string& xml, const std::string& tag);

}  // namespace onvif

}  // namespace ma::node
