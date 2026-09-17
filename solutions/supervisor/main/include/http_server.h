#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "logger.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api_base.h"
#include "api_device.h"
#include "api_file.h"
#include "api_led.h"
#include "api_user.h"
#include "api_wifi.h"
#include "api_halow.h"

class http_server {
public:
    http_server(const std::string& root_dir = "www",
        const std::string& cert = "", const std::string& key = "")
        : _cert(cert.c_str())
        , _key(key.c_str())
        , _root_dir(root_dir.c_str())
    {
        _apis.emplace_back(std::make_unique<api_base>());
        _apis.emplace_back(std::make_unique<api_device>());
        _apis.emplace_back(std::make_unique<api_file>());
        _apis.emplace_back(std::make_unique<api_led>());
        _apis.emplace_back(std::make_unique<api_user>());
        _apis.emplace_back(std::make_unique<api_wifi>());
        _apis.emplace_back(std::make_unique<api_halow>());
        mg_mgr_init(&mgr);
    }

    ~http_server()
    {
        stop();
    }

    bool start(const std::string& http_port = "80", const std::string& https_port = "")
    {
        if (!http_port.empty()) {
            http_conn = mg_http_listen(&mgr, std::string(":" + http_port).c_str(),
                event_handler, this);
            if (!http_conn)
                return false;
            LOGV("HTTP server started on %s", http_port.c_str());
        }
        if (!https_port.empty()) {
            https_conn = mg_http_listen(&mgr, std::string(":" + https_port).c_str(),
                https_event_handler, this);
            if (!https_conn)
                return false;
            LOGV("HTTPS server started on %s", https_port.c_str());
        }

        if (!http_conn && !https_conn) {
            LOGV("Error: At least one valid port required");
            return false;
        }

        start_workers();

        // Drain worker replies on the poll thread. mg_iotest() caps its wait
        // at the next timer deadline, so poll stays efficient with -1.
        mg_timer_add(&mgr, 20, MG_TIMER_REPEAT, drain_replies, this);

        worker = std::thread([this]() {
            running = true;
            signal(SIGUSR1, [](int sig) {
            });
            while (running)
                // cap the poll timeout: with -1, epoll sleeps forever and the
                // reply-drain timer below only fires on unrelated socket
                // events, stalling every queued /api/ response
                mg_mgr_poll(&mgr, 100);
            LOGV("poll_loop exit");
        });

        return true;
    }

    void stop()
    {
        LOGV("");
        stop_workers();
        if (running) {
            running = false;
            pthread_kill(worker.native_handle(), SIGUSR1);
            if (worker.joinable()) {
                worker.join();
            }
            mg_mgr_free(&mgr);
        }
        LOGV("Server stopped");
    }

private:
    const char* _cert;
    const char* _key;
    const char* _root_dir;
    // const std::string _ssi_pattern = "#.html";

    mg_mgr mgr;
    mg_connection* http_conn = nullptr;
    mg_connection* https_conn = nullptr;

    std::atomic<bool> running { false };
    std::thread worker;
    std::vector<std::unique_ptr<api_base>> _apis;

    // ---------------------------------------------------------------
    // worker pool: API handlers call shell scripts that can take
    // seconds; running them on the poll thread serialized every
    // request (one slow /api/ call froze the whole web UI). Workers
    // never touch mongoose: they push finished responses into a
    // mutex-guarded queue that a poll-thread timer sends out.
    // ---------------------------------------------------------------
    static constexpr size_t WORKER_COUNT = 2;
    static constexpr size_t MAX_PENDING_JOBS = 16; // per worker
    // reject oversized uploads with a clean 413 before mongoose's own
    // MG_MAX_RECV_SIZE hard kill (ioalloc() drops the connection with no
    // HTTP reply). MG_EV_HTTP_HDRS fires right after the headers, so a
    // Content-Length above this never gets buffered. Reference the macro
    // so both limits stay in sync if mongoose is reconfigured.
    static constexpr size_t MAX_BODY_SIZE = MG_MAX_RECV_SIZE;

    struct api_job {
        unsigned long conn_id = 0;
        std::string raw; // stable copy of the whole HTTP message
        mg_http_message hm; // copy of hm, pointers relocated into raw
    };

    struct reply {
        unsigned long conn_id;
        int code; // HTTP status code
        std::string headers; // extra headers, each "Key: value\r\n"
        std::string body;
    };

    struct worker_ctx {
        std::deque<api_job> jobs;
        std::mutex mutex;
        std::condition_variable cv;
    };

    std::vector<std::unique_ptr<worker_ctx>> pool_;
    std::vector<std::thread> pool_threads_;
    std::atomic<bool> stopping_ { false };

    std::mutex replies_mutex_;
    std::deque<reply> replies_;

    void start_workers()
    {
        for (size_t i = 0; i < WORKER_COUNT; i++) {
            pool_.emplace_back(std::make_unique<worker_ctx>());
        }
        for (size_t i = 0; i < WORKER_COUNT; i++) {
            pool_threads_.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    void stop_workers()
    {
        stopping_ = true;
        for (auto& ctx : pool_) {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->cv.notify_all();
        }
        for (auto& t : pool_threads_) {
            if (t.joinable())
                t.join();
        }
        pool_threads_.clear();
    }

    // endpoint-specific: mg_http_serve_file() must run on the poll thread
    // (it streams from disk using the connection), keep those inline
    static bool is_inline_uri(const std::string& uri)
    {
        size_t pos = uri.find("/api/");
        if (pos == std::string::npos)
            return false;
        std::string rest = uri.substr(pos + 5);
        return rest.rfind("fileMgr/download", 0) == 0
            || rest.rfind("deviceMgr/getModelFile", 0) == 0;
    }

    static void relocate_mg_str(struct mg_str* s, ptrdiff_t delta)
    {
        if (s->buf != nullptr) {
            *(const char**)&s->buf = s->buf + delta;
        }
    }

    static void relocate_hm(mg_http_message* hm, const char* from, const char* to)
    {
        ptrdiff_t delta = to - from;
        relocate_mg_str(&hm->message, delta);
        relocate_mg_str(&hm->body, delta);
        relocate_mg_str(&hm->head, delta);
        relocate_mg_str(&hm->method, delta);
        relocate_mg_str(&hm->uri, delta);
        relocate_mg_str(&hm->proto, delta);
        relocate_mg_str(&hm->query, delta);
        for (size_t i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
            relocate_mg_str(&hm->headers[i].name, delta);
            relocate_mg_str(&hm->headers[i].value, delta);
        }
    }

    void enqueue_api_job(mg_connection* c, mg_http_message* hm)
    {
        if (hm->body.len > MAX_BODY_SIZE) {
            mg_http_reply(c, 413, "Content-Type: text/plain\r\n", "Payload too large");
            return;
        }

        size_t idx = c->id % WORKER_COUNT; // keeps per-connection ordering
        auto& ctx = *pool_[idx];

        api_job job;
        job.conn_id = c->id;
        job.raw.assign(hm->message.buf, hm->message.len);
        job.hm = *hm;
        relocate_hm(&job.hm, hm->message.buf, job.raw.data());

        {
            std::lock_guard<std::mutex> lock(ctx.mutex);
            if (stopping_ || ctx.jobs.size() >= MAX_PENDING_JOBS) {
                mg_http_reply(c, 503, "Content-Type: text/plain\r\n", "Server busy");
                return;
            }
            ctx.jobs.push_back(std::move(job));
        }
        ctx.cv.notify_one();
    }

    static constexpr const char* CORS_HEADERS
        = "Content-Type: application/json\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
          "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";

    void worker_loop(size_t idx)
    {
        auto& ctx = *pool_[idx];
        while (true) {
            api_job job;
            {
                std::unique_lock<std::mutex> lock(ctx.mutex);
                ctx.cv.wait(lock, [this, &ctx] { return stopping_ || !ctx.jobs.empty(); });
                if (stopping_ && ctx.jobs.empty())
                    return;
                job = std::move(ctx.jobs.front());
                ctx.jobs.pop_front();
            }

            json res;
            // a throw from any api handler must not kill the worker
            api_status_t status = API_STATUS_ERROR;
            try {
                status = api_base::api_handler(&job.hm, res);
            } catch (const std::exception& e) {
                LOGE("API exception: %s", e.what());
            } catch (...) {
                LOGE("API unknown exception");
            }

            // structured result only: the actual bytes must be sent from the
            // poll thread via mg_http_reply(), which manages the mongoose
            // is_resp lifecycle. Raw mg_send() leaves is_resp set and the
            // connection stops parsing the next keep-alive request
            reply r;
            r.conn_id = job.conn_id;
            switch (status) {
            case API_STATUS_OK:
                r.code = 200;
                r.headers = CORS_HEADERS;
                r.body = res.dump();
                break;
            case API_STATUS_UNAUTHORIZED:
                r.code = 401;
                r.headers = "Content-Type: text/plain\r\n";
                r.body = "Unauthorized";
                break;
            case API_STATUS_ERROR:
            default:
                r.code = 500;
                r.headers = "Content-Type: text/plain\r\n";
                r.body = "Internal Server Error";
                break;
            }

            std::lock_guard<std::mutex> lock(replies_mutex_);
            replies_.push_back(std::move(r));
        }
    }

    mg_connection* find_conn(unsigned long id)
    {
        for (mg_connection* c = mgr.conns; c != nullptr; c = c->next) {
            if (c->id == id)
                return c;
        }
        return nullptr;
    }

    // poll-thread timer: send out everything the workers finished
    static void drain_replies(void* arg)
    {
        http_server* server = static_cast<http_server*>(arg);

        std::deque<reply> pending;
        {
            std::lock_guard<std::mutex> lock(server->replies_mutex_);
            pending.swap(server->replies_);
        }

        for (auto& r : pending) {
            mg_connection* c = server->find_conn(r.conn_id);
            if (c != nullptr) {
                // mg_http_reply keeps the mongoose is_resp lifecycle correct,
                // so the connection can parse the next keep-alive request
                mg_http_reply(c, r.code, r.headers.c_str(), "%s", r.body.c_str());
            }
        }
    }

    // ---------------------------------------------------------------
    // event handling (poll thread only)
    // ---------------------------------------------------------------
    static void event_handler(mg_connection* c, int ev, void* ev_data)
    {
        http_server* server = static_cast<http_server*>(c->fn_data);

        if (ev == MG_EV_HTTP_HDRS) {
            // headers complete, body NOT yet buffered: the only point where
            // an oversized upload can be stopped before mongoose OOMs
            // buffering it (mg_iobuf has no size limit of its own)
            mg_http_message* hm = (mg_http_message*)ev_data;
            if (hm->body.len > MAX_BODY_SIZE) {
                LOGW("oversized body %llu bytes, rejecting", (unsigned long long)hm->body.len);
                mg_http_reply(c, 413, "Content-Type: text/plain\r\n", "Payload too large");
                c->is_draining = 1; // close once the reply is flushed
            }
            return;
        }

        if (ev == MG_EV_HTTP_MSG) {
            http_server* server = static_cast<http_server*>(c->fn_data);
            mg_http_message* hm = (mg_http_message*)ev_data;

            LOGV(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
            LOGV("---> uri=%s", std::string(hm->uri.buf, hm->uri.len).c_str());
            LOGV("---> query=%s", std::string(hm->query.buf, hm->query.len).c_str());
            LOGV("---> head=%s", std::string(hm->head.buf, hm->head.len).c_str());
            // LOGV("---> body=%s", std::string(hm->body.buf, hm->body.len).c_str());
            // LOGV(std::string(hm->message.buf, hm->message.len).c_str());
            LOGV("<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n");

            std::string uri(hm->uri.buf, hm->uri.len);

            // API requests go to the worker pool (except file downloads)
            if (uri.find("/api/") != std::string::npos && !is_inline_uri(uri)) {
                server->enqueue_api_job(c, hm);
                return;
            }

            json res;
            // inline paths (downloads / statics) keep the exception guard too
            api_status_t status = API_STATUS_ERROR;
            try {
                status = api_base::api_handler(hm, res);
            } catch (const std::exception& e) {
                LOGE("API exception: %s", e.what());
            } catch (...) {
                LOGE("API unknown exception");
            }
            if (status == API_STATUS_OK) {
                mg_http_reply(c, 200, CORS_HEADERS, "%s", res.dump().c_str());
                return;
            } else if (status == API_STATUS_UNAUTHORIZED) {
                mg_http_reply(c, 401, "Content-Type: text/plain\r\n", "Unauthorized");
                return;
            } else if (status == API_STATUS_REPLY_FILE) {
                std::string fname("");
                try {
                    LOGV("Reply file: %s", res.dump().c_str());
                    fname = res["data"]["file"].get<std::string>();
                } catch (const json::exception& e) {
                    LOGE("json error: %s", e.what());
                }
                if (fname.empty()) {
                    mg_http_reply(c, 400, "Content-Type: text/plain\r\n", "Bad Request");
                    return;
                }
                struct mg_http_serve_opts _opts = { .root_dir = NULL };
                mg_http_serve_file(c, hm, fname.c_str(), &_opts);
                return;
            } else if (status != API_STATUS_NEXT) {
                mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Internal Server Error");
                return;
            }

            // redirection
            if (!(uri == "/"
                    || uri.find(".png") != std::string::npos
                    || uri.find(".html") != std::string::npos
                    || uri.find("assets") != std::string::npos
                    || uri.find("js") != std::string::npos)) {
                std::string redirect;
                mg_str* host = mg_http_get_header(hm, "Host");
                if (host) {
                    redirect.assign(host->buf, host->len);
                }

                if (redirect.empty()) {
                    redirect = "192.168.16.1";
                } else {
                    // is ip address ?
                    std::stringstream ss(redirect);
                    std::string segment;
                    while (std::getline(ss, segment, '.')) {
                        if (segment.empty() || segment.find(":") != std::string::npos)
                            continue;
                        if (!std::all_of(segment.begin(), segment.end(), ::isdigit)) {
                            redirect = "192.168.16.1";
                            break;
                        }
                    }
                }

                redirect = "Location: http://" + redirect + "/\r\n";
                LOGD("redirect============>%s", redirect.c_str());
                mg_http_reply(c, 307, redirect.c_str(), "");
                return;
            }

            // Serve web root directory
            struct mg_http_serve_opts opts = { 0 };
            opts.root_dir = server->_root_dir;
            // opts.ssi_pattern = server->_ssi_pattern;
            mg_http_serve_dir(c, hm, &opts);
        }
    }

    static void https_event_handler(mg_connection* c, int ev, void* ev_data)
    {
        http_server* server = static_cast<http_server*>(c->fn_data);
        if (ev == MG_EV_ACCEPT) {
            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.cert = mg_str(server->_cert);
            opts.key = mg_str(server->_key);
            mg_tls_init(c, &opts);
        } else {
            event_handler(c, ev, ev_data);
        }
    }
};
#endif // HTTP_SERVER_H
