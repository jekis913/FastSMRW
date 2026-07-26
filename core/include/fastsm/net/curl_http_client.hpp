#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fastsm/net/http_client.hpp"

namespace fastsm::net {

// IHttpClient implementation over libcurl (desktop Linux). Synchronous send();
// call only from the core's worker thread. The analogue of WinHttpClient /
// DarwinHttpClient. A single instance is reusable across requests and threads
// (a fresh curl easy handle is created per call).
class CurlHttpClient : public IHttpClient {
public:
    explicit CurlHttpClient(std::string user_agent = "FastSMRW/0.0.1");

    HttpResponse send(const HttpRequest& request) override;
    void send_stream(const HttpRequest& request, const std::function<bool()>& should_continue,
                     const std::function<void(std::string_view)>& on_chunk) override;

    // Flags every in-progress streaming read to abort so a blocked send_stream
    // returns promptly. Safe to call from another thread.
    void cancel_streams() override;

private:
    std::string user_agent_;
    std::mutex stream_mutex_;
    // One cancellation flag per live stream; cancel_streams() sets them all and
    // the stream's own curl callbacks notice and abort the transfer.
    std::vector<std::shared_ptr<std::atomic<bool>>> active_streams_;
};

} // namespace fastsm::net
