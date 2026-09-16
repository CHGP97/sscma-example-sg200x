#pragma once

#include "node.h"
#include "server.h"

#include "video.h"

namespace ma::node {

#define AUDIO_DEVICE "hw:0"
#define SAMPLE_RATE  16000
#define CHANNELS     1
#define FORMAT       SND_PCM_FORMAT_S16_LE

// Channel index == physical VPSS channel == VB pool index.
// Hardware scaler width limits in VI-VPSS online mode are per channel:
//   chn0(sc_d)=1920, chn1(sc_v1)=2880, chn2(sc_v2)=1920  [cvi_cv181x_defines.h]
// A channel asked to output wider than its scaler silently stalls the whole
// pipeline (the kernel tile fallback is disabled for online input), so the
// only channel allowed to carry >1920-wide video (e.g. 2592x1944 H.264 in
// Max mode) is CHN_H264, which MUST stay at index 1.
enum { CHN_RAW = 0, CHN_H264 = 1, CHN_JPEG = 2, CHN_AUDIO = 3, CHN_MAX };

typedef struct {
    int chn;
    int32_t width;
    int32_t height;
    int32_t fps;
    ma_pixel_format_t format;
    bool configured;
    bool enabled;
    bool dropped;
    std::vector<MessageBox*> msgboxes;
} channel;

class Frame {
public:
    Frame() : ref_cnt(0), chn(CHN_MAX) {}
    ~Frame() = default;
    inline void ref(int n = 1) {
        ref_cnt.fetch_add(n, std::memory_order_relaxed);
    }
    virtual inline void release() = 0;
    int chn;
    std::atomic<int> ref_cnt;
    ma_tick_t timestamp;
};

class videoFrame : public Frame {
public:
    videoFrame() : Frame() {
        memset(&img, 0, sizeof(img));
    }
    inline void release() override {
        if (ref_cnt.load(std::memory_order_relaxed) == 0 || ref_cnt.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (!img.physical) {
                delete[] img.data;
            }
            delete this;
        }
    }
    std::vector<std::pair<void*, size_t>> blocks;
    ma_img_t img;
    int fps;
};

class audioFrame : public Frame {
public:
    audioFrame() : Frame() {
        data = nullptr;
        size = 0;
    }
    inline void release() override {
        if (ref_cnt.load(std::memory_order_relaxed) == 0 || ref_cnt.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete[] data;
            delete this;
        }
    }

    uint8_t* data;
    size_t size;
};

class CameraNode : public Node {

public:
    using audioCallback = void (*)(const uint8_t* data, size_t size);
    CameraNode(std::string id);
    ~CameraNode();

    ma_err_t onCreate(const json& config) override;
    ma_err_t onStart() override;
    ma_err_t onControl(const std::string& control, const json& data) override;
    ma_err_t onStop() override;
    ma_err_t onDestroy() override;

    ma_err_t config(int chn, int32_t width = -1, int32_t height = -1, int32_t fps = -1, ma_pixel_format_t format = MA_PIXEL_FORMAT_UNKNOWN, bool enabled = true);
    ma_err_t attach(int chn, MessageBox* msgbox);
    ma_err_t detach(int chn, MessageBox* msgbox);

    // still-capture size chosen by the camera node's resolution option
    // (e.g. 2592x1944 in Max mode), before the model preview reconfigures
    // the JPEG channel to its preview size
    void getCaptureRes(int32_t& w, int32_t& h) const {
        w = capture_w_;
        h = capture_h_;
    }

    // preview size: same field of view as capture but capped to the JPEG
    // channel's scaler limit (sc_v2 = 1920 wide) and to a memory-safe scale.
    // A full 5MP preview pool (~5.5MB/frame after tile compression) together
    // with the other pools exceeds the ION budget and crashes the kernel, so
    // in Max mode the preview runs at an aspect-preserving <=1920 downscale
    // (2592x1944 -> 1440x1080, no letterboxing: both are 4:3)
    void getPreviewRes(int32_t& w, int32_t& h) const {
        if (capture_w_ > 1920) {
            w = 1920;
            h = (int)((int64_t)capture_h_ * 1920 / capture_w_ / 2) * 2;
        } else {
            w = capture_w_;
            h = capture_h_;
        }
    }

protected:
    void threadEntry();
    void threadAudioEntry();
    static void threadEntryStub(void* obj);
    static void threadAudioEntryStub(void* obj);
    int vencCallback(void* pData, void* pArgs);
    int vpssCallback(void* pData, void* pArgs);
    static int vencCallbackStub(void* pData, void* pArgs, void* pUserData);
    static int vpssCallbackStub(void* pData, void* pArgs, void* pUserData);

private:
    std::vector<channel> channels_;
    uint32_t count_;
    bool preview_;
    bool websocket_;
    int audio_;
    int option_;
    int fps_;
    int light_;
    bool mirror_;
    bool flip_;
    int max_w_;      // sensor max capability (valid when max_res_active_)
    int max_h_;
    int max_fps_;
    bool max_res_active_;  // sensor max exceeds the 1080p default
    int32_t capture_w_;    // still-capture size from the resolution option
    int32_t capture_h_;
    Thread* thread_;
    Thread* thread_audio_;
    MessageBox frame_;
    TransportWebSocket* transport_;
};

}  // namespace ma::node