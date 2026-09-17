#include "ma_transport_websocket.h"

#include <sys/socket.h>

namespace ma {

using namespace hv;

static const char* TAG = "ma::transport::websocket";

// per-client send backlog limit (bytes); frames are dropped for clients whose
// pending write queue exceeds this, bounding their stream latency (~2-3 frames)
static constexpr size_t kMaxWriteBacklogBytes = 256 * 1024;

// kernel socket send buffer for streaming clients; kept small so TCP backpressure
// surfaces in the write queue (visible to the drop-frame check above) after
// ~0.3s of frames instead of hiding several MB / seconds of stale frames
static constexpr int kSocketSndBufBytes = 256 * 1024;


TransportWebSocket::TransportWebSocket() : Transport{MA_TRANSPORT_WS}, m_port(8080), m_receiveBuffer(nullptr) {}
TransportWebSocket::~TransportWebSocket() {
    deInit();
}

ma_err_t TransportWebSocket::init(const void* config) noexcept {


    const Config* config_ = reinterpret_cast<const Config*>(config);

    m_server.port = config_->port;

    m_service.onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        setsockopt(channel->fd(), SOL_SOCKET, SO_SNDBUF, &kSocketSndBufBytes, sizeof(kSocketSndBufBytes));
        m_channels.emplace_front(channel);
    };

    m_service.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
        m_receiveBuffer->push(msg.c_str(), msg.length());
    };

    m_service.onclose = [this](const WebSocketChannelPtr& channel) {
        m_channels.remove_if([](const WebSocketChannelPtr& c) { return !c->isConnected(); });
    };

    m_server.registerWebSocketService(&m_service);

    m_receiveBuffer = new SPSCRingBuffer<char>(64 * 1024);

    if (m_server.start() == MA_OK) {
        return MA_OK;
    }

    return MA_OK;
}
void TransportWebSocket::deInit() noexcept {

    m_server.stop();

    if (m_receiveBuffer) {
        delete m_receiveBuffer;
        m_receiveBuffer = nullptr;
    }
}

size_t TransportWebSocket::available() const noexcept {
    Guard guard(m_mutex);
    return m_receiveBuffer->size();
}

size_t TransportWebSocket::flush() noexcept {
    Guard guard(m_mutex);
    m_receiveBuffer->clear();
    return 0;
}

size_t TransportWebSocket::send(const char* data, size_t length) noexcept {
    Guard guard(m_mutex);
    if (m_channels.empty()) {
        return 0;
    }
    bool sent = false;
    for (auto& channel : m_channels) {
        if (!channel->isConnected()) {
            continue;
        }
        // Each frame is independently renderable, so a client whose socket
        // backlog already exceeds the threshold is skipped for this frame
        // instead of queueing behind stale data: slow consumers fall behind by
        // at most ~kMaxWriteBacklogBytes of stream rather than accumulating
        // unbounded latency up to libhv's 16MB limit (which drops the
        // connection). H264 streams resume at their next keyframe after a skip.
        if (channel->writeBufsize() > kMaxWriteBacklogBytes) {
            continue;
        }
        channel->send(data, length);
        sent = true;
    }
    return sent ? length : 0;
}

size_t TransportWebSocket::receive(char* data, size_t length) noexcept {
    Guard guard(m_mutex);
    if (m_receiveBuffer->empty()) {
        return 0;
    }
    return m_receiveBuffer->pop(data, length);
}

size_t TransportWebSocket::receiveIf(char* data, size_t length, char delimiter) noexcept {
    Guard guard(m_mutex);
    if (m_receiveBuffer->empty()) {
        return 0;
    }
    return m_receiveBuffer->popIf(data, length, delimiter);
}

}  // namespace ma