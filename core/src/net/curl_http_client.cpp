#include "fastsm/net/curl_http_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>

#include "fastsm/util/log.hpp"

namespace fastsm::net {
namespace {

// curl_global_init is not thread-safe, so run it exactly once before the first
// handle is created. Never cleaned up (lives for the process, like WinHTTP).
void global_init_once() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool has_header(const Headers& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (key.size() == name.size() &&
            std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }))
            return true;
    }
    return false;
}

// Collects "Key: Value" response headers, restarting on each new status line so
// only the final response's headers survive redirects.
size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<Headers*>(userdata);
    const size_t len = size * nitems;
    std::string_view line(buffer, len);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();
        return len;
    }
    if (const size_t colon = line.find(':'); colon != std::string_view::npos) {
        std::string_view key = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        headers->emplace_back(std::string(key), std::string(value));
    }
    return len;
}

size_t body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Options shared by send() and send_stream().
void set_common_options(CURL* curl, const HttpRequest& req, const std::string& user_agent,
                        curl_slist* headers) {
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    // The core runs requests on several threads; without NOSIGNAL curl's DNS
    // timeouts use SIGALRM, which is unsafe multithreaded.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // WinHTTP and NSURLSession follow redirects by default; curl does not.
    // Update downloads rely on this (GitHub release assets 302 to a CDN).
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!has_header(req.headers, "User-Agent"))
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
}

curl_slist* build_header_list(const Headers& headers) {
    curl_slist* list = nullptr;
    for (const auto& [key, value] : headers)
        list = curl_slist_append(list, (key + ": " + value).c_str());
    return list;
}

// State shared with the streaming callbacks. Chunks are only delivered once the
// response status is known to be 2xx (a 401/404 body must not reach the SSE
// parser), and both callbacks abort the transfer as soon as the stream is
// cancelled or the driver stops asking for data.
struct StreamCtx {
    CURL* curl = nullptr;
    const std::function<bool()>* should_continue = nullptr;
    const std::function<void(std::string_view)>* on_chunk = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled;
    bool status_checked = false;
    bool status_ok = false;
};

size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    const size_t len = size * nmemb;
    if (ctx->cancelled->load() || !(*ctx->should_continue)())
        return 0; // abort the transfer
    if (!ctx->status_checked) {
        long code = 0;
        curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &code);
        ctx->status_ok = code >= 200 && code < 300;
        ctx->status_checked = true;
    }
    if (!ctx->status_ok)
        return 0; // unusable stream (401/404/...): drop it, driver will retry
    (*ctx->on_chunk)(std::string_view(ptr, len));
    return len;
}

// Fires about once a second even when no data arrives, so a cancelled stream
// unblocks promptly instead of waiting for the next keep-alive.
int stream_progress_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    return (ctx->cancelled->load() || !(*ctx->should_continue)()) ? 1 : 0;
}

} // namespace

CurlHttpClient::CurlHttpClient(std::string user_agent) : user_agent_(std::move(user_agent)) {
    global_init_once();
}

HttpResponse CurlHttpClient::send(const HttpRequest& req) {
    HttpResponse res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        res.error = "curl_easy_init failed";
        return res;
    }

    char errbuf[CURL_ERROR_SIZE] = {};
    curl_slist* headers = build_header_list(req.headers);
    set_common_options(curl, req, user_agent_, headers);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    // No overall time cap (a multi-MB media upload can legitimately take
    // minutes on a slow uplink); instead abort a transfer that has fully
    // stalled for 60s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    // Ask for (and transparently decode) compressed responses.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        res.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        res.status = 0;
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        res.status = code;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

void CurlHttpClient::send_stream(const HttpRequest& req,
                                 const std::function<bool()>& should_continue,
                                 const std::function<void(std::string_view)>& on_chunk) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return;

    StreamCtx ctx;
    ctx.curl = curl;
    ctx.should_continue = &should_continue;
    ctx.on_chunk = &on_chunk;
    ctx.cancelled = std::make_shared<std::atomic<bool>>(false);
    // Register so cancel_streams() can flag this transfer to abort.
    {
        std::lock_guard<std::mutex> lk(stream_mutex_);
        active_streams_.push_back(ctx.cancelled);
    }
    // Deregister exactly once (cancel_streams may have already dropped it).
    auto cleanup = [&] {
        std::lock_guard<std::mutex> lk(stream_mutex_);
        auto it = std::find(active_streams_.begin(), active_streams_.end(), ctx.cancelled);
        if (it != active_streams_.end())
            active_streams_.erase(it);
    };

    curl_slist* headers = build_header_list(req.headers);
    set_common_options(curl, req, user_agent_, headers);
    // Mastodon sends keep-alives every ~30s, so 60s of total silence means the
    // connection is dead; returning lets the client reconnect. No compression:
    // SSE events must reach the parser as they arrive, not sit in a decoder.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    cleanup();
}

void CurlHttpClient::cancel_streams() {
    std::lock_guard<std::mutex> lk(stream_mutex_);
    if (!active_streams_.empty())
        fastsm::log::write("curl: cancel_streams flagging " +
                           std::to_string(active_streams_.size()) + " active stream(s)");
    for (const auto& flag : active_streams_)
        flag->store(true);
    active_streams_.clear();
}

} // namespace fastsm::net
