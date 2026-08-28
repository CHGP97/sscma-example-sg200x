#include "onvif_utils.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "core/ma_debug.h"
#include "version.h"

namespace ma::node {

static constexpr char TAG[] = "ma::node::onvif";

namespace onvif {

const std::string& serialNumber() {
    static std::string sn;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        char buf[128] = {0};
        if (FILE* fp = popen("fw_printenv -n sn 2>/dev/null", "r")) {
            if (fgets(buf, sizeof(buf), fp)) {
                char* p = buf + strlen(buf);
                while (p > buf && (p[-1] == '\n' || p[-1] == '\r')) {
                    *--p = '\0';
                }
            }
            pclose(fp);
        }
        if (buf[0] != '\0') {
            sn = buf;
        } else {
            char hostname[64] = {0};
            gethostname(hostname, sizeof(hostname) - 1);
            sn = "recamera-";
            sn += hostname;
            MA_LOGW(TAG, "sn not available, falling back to %s", sn.c_str());
        }
    }
    return sn;
}

const std::string& firmwareVersion() {
    static const std::string version = PROJECT_VERSION;
    return version;
}

const std::string& model() {
    static const std::string m = "ReCamera";
    return m;
}

const std::string& endpointUUID() {
    // Deterministic urn:uuid from SHA-1(serial): stable across reboots so
    // clients keep matching the device after rediscovery.
    static std::string uuid;
    if (uuid.empty()) {
        unsigned char digest[20];
        sha1("recamera-endpoint:" + serialNumber(), digest);
        digest[6] = (digest[6] & 0x0f) | 0x50;  // version 5
        digest[8] = (digest[8] & 0x3f) | 0x80;  // RFC 4122 variant
        char out[40];
        for (int i = 0; i < 16; ++i) {
            snprintf(out + i * 2, 3, "%02x", digest[i]);
        }
        uuid = "urn:uuid:";
        uuid.append(out, 8).append("-").append(out + 8, 4).append("-").append(out + 12, 4).append("-").append(out + 16, 4).append("-").append(out + 20, 12);
    }
    return uuid;
}

std::string localIP() {
    std::string ip;
    struct ifaddrs* ifas = nullptr;
    if (getifaddrs(&ifas) != 0 || ifas == nullptr) {
        return ip;
    }
    for (struct ifaddrs* ifa = ifas; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK || !(ifa->ifa_flags & IFF_UP)) {
            continue;
        }
        char addr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr, addr, sizeof(addr));
        if (addr[0] != '\0' && strcmp(addr, "0.0.0.0") != 0) {
            ip = addr;
            break;
        }
    }
    freeifaddrs(ifas);
    return ip;
}

std::string hostname() {
    char buf[256] = {0};
    gethostname(buf, sizeof(buf) - 1);
    return buf;
}

void systemTime(int& year, int& mon, int& day, int& hour, int& min, int& sec, long& utcOffset) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm utc;
    gmtime_r(&tv.tv_sec, &utc);
    struct tm local;
    localtime_r(&tv.tv_sec, &local);
    year = utc.tm_year + 1900;
    mon  = utc.tm_mon + 1;
    day  = utc.tm_mday;
    hour = utc.tm_hour;
    min  = utc.tm_min;
    sec  = utc.tm_sec;
    utcOffset = local.tm_gmtoff;
}

std::string base64Encode(const unsigned char* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += table[(v >> 18) & 0x3f];
        out += table[(v >> 12) & 0x3f];
        out += table[(v >> 6) & 0x3f];
        out += table[v & 0x3f];
    }
    if (i + 1 == len) {
        uint32_t v = data[i] << 16;
        out += table[(v >> 18) & 0x3f];
        out += table[(v >> 12) & 0x3f];
        out += "==";
    } else if (i + 2 == len) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += table[(v >> 18) & 0x3f];
        out += table[(v >> 12) & 0x3f];
        out += table[(v >> 6) & 0x3f];
        out += '=';
    }
    return out;
}

void sha1(const std::string& in, unsigned char out[20]) {
    SHA1(reinterpret_cast<const unsigned char*>(in.data()), in.size(), out);
}

std::string base64Decode(const std::string& in) {
    static auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3 + 3);
    int group[4];
    int n = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        int v = value(c);
        if (v < 0) {
            continue;
        }
        group[n++] = v;
        if (n == 4) {
            out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
            out += static_cast<char>(((group[1] & 0x0f) << 4) | (group[2] >> 2));
            out += static_cast<char>(((group[2] & 0x03) << 6) | group[3]);
            n = 0;
        }
    }
    if (n == 2) {
        out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
    } else if (n == 3) {
        out += static_cast<char>((group[0] << 2) | (group[1] >> 4));
        out += static_cast<char>(((group[1] & 0x0f) << 4) | (group[2] >> 2));
    }
    return out;
}

std::string xmlText(const std::string& xml, const std::string& tag) {
    static auto isBoundary = [](char c) {
        return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
    };
    size_t pos = 0;
    while ((pos = xml.find(tag, pos)) != std::string::npos) {
        // must start a tag name: directly after '<' or after a namespace prefix ':'
        if (pos == 0 || (xml[pos - 1] != '<' && xml[pos - 1] != ':')) {
            ++pos;
            continue;
        }
        // reject longer names ("Created" must not match "CreatedAt")
        if (pos + tag.size() < xml.size() && !isBoundary(xml[pos + tag.size()])) {
            ++pos;
            continue;
        }
        // reject closing tags ("</tag>" / "</pre:tag>")
        bool closing = false;
        if (xml[pos - 1] == ':') {
            size_t p = pos - 1;
            while (p > 0 && xml[p - 1] != '<') {
                --p;
            }
            closing = (p == 0) || xml[p] == '/';
        }
        if (closing) {
            ++pos;
            continue;
        }
        size_t gt = xml.find('>', pos + tag.size());
        if (gt == std::string::npos) {
            break;
        }
        if (gt > pos + tag.size() && xml[gt - 1] == '/') {
            ++pos;  // self-closing, no text
            continue;
        }
        size_t end = xml.find("</", gt);
        if (end == std::string::npos) {
            break;
        }
        return xml.substr(gt + 1, end - gt - 1);
    }
    return "";
}

bool xmlHas(const std::string& xml, const std::string& tag) {
    return xml.find(tag) != std::string::npos;
}

}  // namespace onvif

}  // namespace ma::node
