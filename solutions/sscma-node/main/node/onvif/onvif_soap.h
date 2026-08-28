#pragma once

#include <string>

namespace ma::node {

namespace onvif {

// Dispatches one ONVIF SOAP request (raw POST body plus the SOAPAction /
// Content-Type headers when present) and returns the full XML response
// envelope. Implements the Device + Media subset needed by ONVIF clients
// and NVRs, with WS-UsernameToken authentication.
std::string handleSoap(const std::string& body, const std::string& soapAction, const std::string& contentType);

}  // namespace onvif

}  // namespace ma::node
