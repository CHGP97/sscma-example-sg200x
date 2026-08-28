#include "onvif_service.h"

#include <cstring>
#include <strings.h>

#include "core/ma_debug.h"

#include "onvif_discovery.h"
#include "onvif_soap.h"
#include "onvif_utils.h"

namespace ma::node {

static constexpr char TAG[] = "ma::node::onvif";

namespace onvif {

Service& Service::instance() {
    static Service svc;
    return svc;
}

Service::Service() : mutex_(true) {}

Service::~Service() {
    stop();
}

// Runs on an hv worker thread; must never block on mutex_ for long and must
// not be invoked while Service::stop() holds the lifecycle state.
static int soapHandler(HttpRequest* req, HttpResponse* resp) {
    resp->headers["Content-Type"] = "application/soap+xml; charset=utf-8";
    std::string soapAction;
    std::string contentType;
    for (const auto& h : req->headers) {
        if (strcasecmp(h.first.c_str(), "soapaction") == 0) {
            soapAction = h.second;
        } else if (strcasecmp(h.first.c_str(), "content-type") == 0) {
            contentType = h.second;
        }
    }
    resp->body = handleSoap(req->body, soapAction, contentType);
    return 200;
}

// Called without mutex_ held: it joins threads whose handlers take mutex_.
bool Service::start(int port) {
    Guard guard(mutex_);
    if (running_) {
        return true;
    }

    httpService_.pathHandlers.clear();
    httpService_.POST("/onvif/device_service", soapHandler);
    // some clients insist on the media-specific path; serve both
    httpService_.POST("/onvif/media_service", soapHandler);

    auto* server = new hv::HttpServer(&httpService_);
    server->setPort(port);
    server->setThreadNum(1);
    if (server->start() != 0) {
        MA_LOGE(TAG, "http server failed to start on port %d", port);
        delete server;
        return false;
    }
    server_.reset(server);

    discovery_.reset(new OnvifDiscovery());
    if (discovery_->start() != 0) {
        MA_LOGW(TAG, "ws-discovery failed to start (SOAP still available)");
        discovery_.reset();
    }

    running_ = true;
    port_    = port;
    MA_LOGI(TAG, "onvif service started on port %d", port);
    return true;
}

// Called without mutex_ held (joins threads whose handlers take mutex_).
void Service::stop() {
    std::unique_ptr<hv::HttpServer> server;
    std::unique_ptr<OnvifDiscovery> discovery;
    {
        Guard guard(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
        port_    = 0;
        server       = std::move(server_);
        discovery    = std::move(discovery_);
    }
    if (discovery) {
        discovery->stop();
    }
    if (server) {
        server->stop();
    }
    MA_LOGI(TAG, "onvif service stopped");
}

ma_err_t Service::registerProfile(const Profile& profile, int port) {
    bool needStart = false;
    {
        Guard guard(mutex_);
        for (const Profile& p : profiles_) {
            if (p.token == profile.token) {
                MA_LOGW(TAG, "profile %s already registered", profile.token.c_str());
                return MA_EBUSY;
            }
        }
        if (running_ && port != port_) {
            MA_LOGW(TAG, "onvif already running on port %d, ignoring requested port %d", port_, port);
        }
        needStart = !running_;
        // publish the profile before the endpoints come up
        profiles_.push_back(profile);
    }
    if (needStart && !start(port)) {
        Guard guard(mutex_);
        profiles_.pop_back();
        return MA_EINVAL;
    }
    MA_LOGI(TAG, "profile %s registered (%dx%d@%d, rtsp:%d%s)", profile.token.c_str(), profile.width, profile.height, profile.fps, profile.rtspPort, profile.rtspPath.c_str());
    return MA_OK;
}

void Service::unregisterProfile(const std::string& token) {
    bool needStop = false;
    {
        Guard guard(mutex_);
        for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
            if (it->token == token) {
                profiles_.erase(it);
                MA_LOGI(TAG, "profile %s unregistered", token.c_str());
                break;
            }
        }
        needStop = running_ && profiles_.empty();
    }
    if (needStop) {
        stop();
    }
}

void Service::updateGeometry(const std::string& token, int width, int height, int fps) {
    Guard guard(mutex_);
    for (Profile& p : profiles_) {
        if (p.token == token) {
            if (p.width != width || p.height != height || p.fps != fps) {
                p.width  = width;
                p.height = height;
                p.fps    = fps;
                MA_LOGI(TAG, "profile %s geometry %dx%d@%d", token.c_str(), width, height, fps);
            }
            return;
        }
    }
}

std::vector<Profile> Service::profiles() {
    Guard guard(mutex_);
    return profiles_;
}

bool Service::running() {
    Guard guard(mutex_);
    return running_;
}

int Service::port() {
    Guard guard(mutex_);
    return port_;
}

std::string Service::deviceServiceUrl() {
    int port;
    {
        Guard guard(mutex_);
        if (!running_) {
            return "";
        }
        port = port_;
    }
    std::string ip = localIP();
    if (ip.empty()) {
        ip = hostname();
    }
    return "http://" + ip + ":" + std::to_string(port) + "/onvif/device_service";
}

}  // namespace onvif

}  // namespace ma::node
