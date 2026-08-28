#include "onvif_discovery.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/ma_debug.h"

#include "onvif_service.h"
#include "onvif_utils.h"

namespace ma::node {

static constexpr char TAG[] = "ma::node::onvif";

static const char* WSDD_GROUP = "239.255.255.250";
static const int WSDD_PORT    = 3702;

OnvifDiscovery::~OnvifDiscovery() {
    stop();
}

int OnvifDiscovery::start() {
    if (running_.load()) {
        return 0;
    }

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        MA_LOGE(TAG, "discovery socket failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(WSDD_PORT);
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        MA_LOGE(TAG, "discovery bind failed: %s", strerror(errno));
        close(fd_);
        fd_ = -1;
        return -1;
    }

    // Join the multicast group on every non-loopback IPv4 interface; joining
    // with INADDR_ANY only is unreliable on multi-interface devices.
    int joined = 0;
    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) == 0) {
        for (struct ifaddrs* ifa = ifas; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if (ifa->ifa_flags & IFF_LOOPBACK || !(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST)) {
                continue;
            }
            struct ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = inet_addr(WSDD_GROUP);
            mreq.imr_interface        = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
            if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0) {
                ++joined;
            }
        }
        freeifaddrs(ifas);
    }
    if (joined == 0) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(WSDD_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }

    // Receive timeout so the loop can observe stop().
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    running_.store(true);
    thread_ = new Thread("onvif#wsdd", threadEntryStub);
    thread_->start(this);

    MA_LOGI(TAG, "ws-discovery listening on udp://%s:%d", WSDD_GROUP, WSDD_PORT);
    return 0;
}

void OnvifDiscovery::stop() {
    if (!running_.load() && fd_ < 0) {
        return;
    }
    running_.store(false);
    if (thread_ != nullptr) {
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
    if (fd_ >= 0) {
        // leave the groups we joined (kernel drops them on close anyway)
        close(fd_);
        fd_ = -1;
    }
}

void OnvifDiscovery::threadEntryStub(void* obj) {
    reinterpret_cast<OnvifDiscovery*>(obj)->threadEntry();
}

void OnvifDiscovery::threadEntry() {
    char buf[8192];
    while (running_.load()) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(fd_, buf, sizeof(buf) - 1, 0, reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (n <= 0) {
            continue;  // timeout or spurious wake, re-check running_
        }
        buf[n] = '\0';
        std::string probe(buf, n);
        if (probe.find("Probe") == std::string::npos || probe.find("ProbeMatch") != std::string::npos || probe.find("Resolve") != std::string::npos) {
            continue;
        }

        const std::string ip = onvif::localIP();
        if (ip.empty()) {
            continue;
        }
        onvif::Service& svc = onvif::Service::instance();
        if (!svc.running()) {
            continue;
        }

        static uint32_t msgNo = 0;
        struct timeval tv;
        gettimeofday(&tv, nullptr);

        std::string relatesTo = onvif::xmlText(probe, "MessageID");
        std::string scopes    = "onvif://www.onvif.org/name/ReCamera onvif://www.onvif.org/hardware/ReCamera onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/type/NetworkVideoTransmitter";

        char match[1536];
        int len = snprintf(match,
                           sizeof(match),
                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                           "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
                           "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
                           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
                           "<s:Header>"
                           "<a:MessageID>urn:uuid:%08x-0000-4000-8000-%012llx</a:MessageID>"
                           "<a:RelatesTo>%s</a:RelatesTo>"
                           "<d:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/>"
                           "</s:Header>"
                           "<s:Body><d:ProbeMatches><d:ProbeMatch>"
                           "<a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference>"
                           "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
                           "<d:Scopes>%s</d:Scopes>"
                           "<d:XAddrs>%s</d:XAddrs>"
                           "<d:MetadataVersion>1</d:MetadataVersion>"
                           "</d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>",
                           (unsigned)(tv.tv_sec & 0xffffffff),
                           (unsigned long long)tv.tv_sec,
                           relatesTo.c_str(),
                           (unsigned)tv.tv_sec,
                           ++msgNo,
                           onvif::endpointUUID().c_str(),
                           scopes.c_str(),
                           svc.deviceServiceUrl().c_str());

        if (len <= 0) {
            continue;
        }
        sendto(fd_, match, len, 0, reinterpret_cast<struct sockaddr*>(&from), fromLen);
        MA_LOGD(TAG, "probe from %s:%d answered", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    }
}

}  // namespace ma::node
