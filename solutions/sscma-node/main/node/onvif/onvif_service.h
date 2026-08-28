#pragma once

#include <memory>
#include <string>
#include <vector>

#include "hv/HttpServer.h"
#include "hv/HttpService.h"

#include "core/ma_types.h"
#include "porting/ma_osal.h"

namespace ma::node {

class OnvifDiscovery;

namespace onvif {

// One ONVIF media profile, backed by one stream node's RTSP session.
struct Profile {
    std::string token;   // RTSP session name, unique per RTSP port
    std::string rtspPath;  // e.g. "/live"
    int rtspPort   = 554;
    std::string username;
    std::string password;
    int width  = 1920;
    int height = 1080;
    int fps    = 30;
};

// Process-wide ONVIF device service. The first stream node registered with
// ONVIF enabled starts the SOAP server (libhv) and the WS-Discovery
// responder; every registration adds a media profile, the last
// unregistration shuts everything down. Mirrors the shared-context pattern
// of ma::TransportRTSP.
class Service {
public:
    static Service& instance();

    // Registers `profile` and starts the service on `port` when idle.
    ma_err_t registerProfile(const Profile& profile, int port);
    void unregisterProfile(const std::string& token);
    void updateGeometry(const std::string& token, int width, int height, int fps);

    // Snapshot accessors for the SOAP / discovery handlers.
    std::vector<Profile> profiles();
    bool running();
    int port();
    std::string deviceServiceUrl();

private:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    bool start(int port);
    void stop();

    Mutex mutex_;
    std::vector<Profile> profiles_;
    bool running_ = false;
    int port_ = 0;

    HttpService httpService_;
    std::unique_ptr<hv::HttpServer> server_;
    std::unique_ptr<OnvifDiscovery> discovery_;
};

}  // namespace onvif

}  // namespace ma::node
