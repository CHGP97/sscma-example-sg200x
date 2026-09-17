#ifndef SERVICED_H
#define SERVICED_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "api_base.h"
#include "logger.hpp"

class serviced {
    typedef enum {
        STATUS_NORMAL,
        STATUS_STARTING,
        STATUS_FAILED,
        STATUS_NORESPONSE,
        STATUS_UNKOWN,

        STATUS_MAX
    } status_t;

public:
    serviced()
    {
        LOGV("");
        thread_ = std::thread([this]() {
            bool sys_booting = true;
            bool restart_flow = false;
            uint32_t nodered_failed = 0;
            uint32_t sscma_failed = 0;

            // consecutive-failure thresholds (1 check ~= 6s)
            static constexpr uint32_t NODERED_DEAD_RESTART = 3; // process exited, restart fast (~18s)
            static constexpr uint32_t NODERED_SLOW_WARN = 10; // alive but stuck, warn (~60s)
            static constexpr uint32_t NODERED_SLOW_RESTART = 30; // stuck too long, restart (~3min)
            static constexpr uint32_t SSCMA_RESTART = 5; // debounce transient mqtt timeouts (~30s)

            running_ = true;
            while (running_) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (cv_.wait_for(lock, std::chrono::seconds(6), [this] { return !running_; })) {
                    break;
                }
                query_nodered();
                query_sscma();
                LOGV("nodered_status_=%d, sscma_status_=%d", nodered_status_, sscma_status_);

                if (sys_booting && (api_base::uptime() < 2 * 60 * 1000)) {
                    continue;
                }
                sys_booting = false;

                if (nodered_status_ != STATUS_NORMAL) {
                    if (0 == nodered_failed) {
                        LOGW("nodered unhealthy (%s)",
                            nodered_dead_ ? "process dead" : "not responding");
                    }
                    ++nodered_failed;
                    if (nodered_dead_) {
                        if (nodered_failed >= NODERED_DEAD_RESTART) {
                            start_service("nodered");
                            nodered_failed = 0;
                        }
                    } else {
                        // node-red needs ~70s to start on this board, keep the
                        // restart threshold well above it to avoid kill loops
                        if (nodered_failed == NODERED_SLOW_WARN) {
                            LOGW("nodered not responding for %d checks", nodered_failed);
                        }
                        if (nodered_failed >= NODERED_SLOW_RESTART) {
                            LOGW("nodered not responding too long, restart");
                            start_service("nodered");
                            nodered_failed = 0;
                        }
                    }
                    continue;
                }
                nodered_failed = 0;

                if (sscma_status_ != STATUS_NORMAL) {
                    if (0 == sscma_failed) {
                        LOGW("sscma unhealthy");
                    }
                    // debounce: a single mqtt timeout should not restart the service
                    if (++sscma_failed >= SSCMA_RESTART) {
                        start_service("sscma");
                        restart_flow = true;
                        sscma_failed = 0;
                    }
                    continue; // To check sscma ready
                }
                sscma_failed = 0;
                if (restart_flow) {
                    LOGW("Restart flow");
                    api_base::script("ctrl_flow", "stop");
                    api_base::script("ctrl_flow", "start");
                    restart_flow = false;
                }
            }
            LOGV("serviced thread exit");
        });
    }

    ~serviced()
    {
        running_ = false;
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        LOGV("");
    }

    status_t get_sscma_status() { return sscma_status_; }
    status_t get_nodered_status() { return nodered_status_; }
    // status_t get_flow_status() { return flow_status_; }

private:
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    std::condition_variable cv_;

    status_t sscma_status_ = STATUS_UNKOWN;
    status_t nodered_status_ = STATUS_UNKOWN;
    bool nodered_dead_ = false;
    // status_t flow_status_ = STATUS_UNKOWN;

    void query_sscma()
    {
        sscma_status_ = STATUS_NORMAL;
        std::string result = api_base::script(__func__);
        if (result.empty() || (result == "Timed out") || (result == "Failed")) {
            sscma_status_ = STATUS_FAILED;
            return;
        }
    }

    // void query_flow()
    // {
    //     flow_status_ = STATUS_NORMAL;
    //     std::string result = api_base::script(__func__);
    //     if (result.empty() || result == "Failed") {
    //         flow_status_ = STATUS_FAILED;
    //         return;
    //     }
    // }

    void query_nodered()
    {
        nodered_status_ = STATUS_NORMAL;
        std::string result = api_base::script(__func__);
        if (result == "Dead") {
            nodered_dead_ = true;
            nodered_status_ = STATUS_FAILED;
            return;
        }
        nodered_dead_ = false;
        if (result.empty() || result != "OK") {
            nodered_status_ = STATUS_FAILED;
            return;
        }
    }

    void start_service(const std::string& service)
    {
        LOGW("start service %s", service.c_str());
        std::string result = api_base::script(__func__, service);
        if (result.empty() || result != "OK") {
            LOGE("start service %s failed", service.c_str());
            return;
        }
    }
};

#endif // SERVICED_H
