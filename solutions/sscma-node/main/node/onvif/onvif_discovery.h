#pragma once

#include <atomic>
#include <string>

#include "porting/ma_osal.h"

namespace ma::node {

// WS-Discovery responder: listens on UDP 239.255.255.250:3702 and answers
// Probe messages with a ProbeMatches pointing at the local ONVIF device
// service. Unicast replies only (permitted by SOAP-over-UDP), so no
// multicast rate control is needed.
class OnvifDiscovery {
public:
    OnvifDiscovery() = default;
    ~OnvifDiscovery();

    // Joins the multicast group and spawns the receive thread.
    // Returns 0 on success.
    int start();

    // Leaves the group and joins the thread. Safe to call repeatedly.
    void stop();

private:
    void threadEntry();
    static void threadEntryStub(void* obj);

    int fd_ = -1;
    Thread* thread_ = nullptr;
    std::atomic<bool> running_{false};
};

}  // namespace ma::node
