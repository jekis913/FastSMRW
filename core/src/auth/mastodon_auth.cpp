#include "fastsm/auth/mastodon_auth.hpp"

// The OAuth flow is split into two portable steps — begin_login (register the
// app + build the authorize URL for a given redirect URI) and finish_login
// (exchange the code + verify) — so any front end can drive its own redirect
// handling. The desktop login() below wraps them around a 127.0.0.1 loopback
// listener that catches the browser redirect; that listener exists on desktop
// platforms (Windows + Linux). Android drives a Custom Tab + fastsm://oauth
// deep link and calls the two helpers directly; Apple front ends do the same
// with the fastsm:// URL scheme.

// Desktop platforms where the in-core loopback redirect listener is built.
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#define FASTSM_HAVE_LOOPBACK_LISTENER 1
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(FASTSM_HAVE_LOOPBACK_LISTENER)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <random>
#include <string>
#include <vector>

#include "fastsm/platform/mastodon/mastodon_map.hpp"
#include "fastsm/util/url.hpp"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

using nlohmann::json;

namespace fastsm {

namespace {

// "push" is required for /api/v1/push/subscription (Web Push notifications);
// without it that endpoint 403s. Adding a scope means existing accounts must be
// re-authorized to obtain a token that carries it.
constexpr const char* kScopes = "read write follow push";
constexpr const char* kClientName = "FastSMRW";

std::string random_state() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 24; ++i)
        s.push_back(hex[dist(rd)]);
    return s;
}

} // namespace

#ifdef FASTSM_HAVE_LOOPBACK_LISTENER
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSock = INVALID_SOCKET;
void close_socket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSock = -1;
void close_socket(SocketHandle s) { ::close(s); }
#endif

// A one-shot loopback HTTP listener: binds 127.0.0.1 on an OS-chosen port,
// hands back the port, then blocks until the browser hits the redirect and
// returns the `code` query parameter.
class LoopbackListener {
public:
    bool start() {
#ifdef _WIN32
        if (WSAStartup(MAKEWORD(2, 2), &wsa_) != 0)
            return false;
        started_ = true;
#endif
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == kInvalidSock)
            return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0; // OS picks
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            return false;
        port_ = ntohs(addr.sin_port);
        return listen(sock_, 1) == 0;
    }

    int port() const { return port_; }
    std::string redirect_uri() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/";
    }

    // Blocks for the redirect; returns the authorization code (or empty).
    std::string wait_for_code() {
        SocketHandle client = accept(sock_, nullptr, nullptr);
        if (client == kInvalidSock)
            return {};
        std::string request;
        char buf[2048];
        int n = static_cast<int>(recv(client, buf, sizeof(buf) - 1, 0));
        if (n > 0)
            request.assign(buf, static_cast<size_t>(n));

        const char* body =
            "<html><head><meta charset=\"utf-8\"><title>FastSMRW</title></head>"
            "<body><h2>FastSMRW</h2><p>Authorization complete. You can close this "
            "window and return to FastSMRW.</p></body></html>";
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                               "Connection: close\r\nContent-Length: " +
                               std::to_string(std::char_traits<char>::length(body)) + "\r\n\r\n" +
                               body;
        send(client, response.c_str(), static_cast<int>(response.size()), 0);
        close_socket(client);

        return extract_code(request);
    }

    ~LoopbackListener() {
        if (sock_ != kInvalidSock)
            close_socket(sock_);
#ifdef _WIN32
        if (started_)
            WSACleanup();
#endif
    }

private:
    static std::string extract_code(const std::string& request) {
        // First line: "GET /?code=XXX&state=YYY HTTP/1.1"
        const size_t q = request.find("code=");
        if (q == std::string::npos)
            return {};
        size_t start = q + 5;
        size_t end = start;
        while (end < request.size() && request[end] != '&' && request[end] != ' ' &&
               request[end] != '\r' && request[end] != '\n')
            ++end;
        return request.substr(start, end - start);
    }

#ifdef _WIN32
    WSADATA wsa_{};
    bool started_ = false;
#endif
    SocketHandle sock_ = kInvalidSock;
    int port_ = 0;
};

} // namespace
#endif // FASTSM_HAVE_LOOPBACK_LISTENER

MastodonAuth::MastodonAuth(net::IHttpClient* http) : http_(http) {}

std::string MastodonAuth::normalize_instance(std::string input) {
    // Trim whitespace.
    size_t a = input.find_first_not_of(" \t\r\n");
    size_t b = input.find_last_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    input = input.substr(a, b - a + 1);
    // Strip scheme.
    if (input.rfind("https://", 0) == 0)
        input = input.substr(8);
    else if (input.rfind("http://", 0) == 0)
        input = input.substr(7);
    // Strip trailing slashes and any path.
    if (size_t slash = input.find('/'); slash != std::string::npos)
        input = input.substr(0, slash);
    return input.empty() ? std::string{} : "https://" + input;
}

// Portable step 1: register the app for `redirect_uri` and build the authorize
// URL. The caller sends the user to authorize_url, then hands the returned code
// (with credentials) to finish_login().
MastodonBeginResult MastodonAuth::begin_login(const std::string& instance_input,
                                              const std::string& redirect_uri) {
    MastodonBeginResult result;
    const std::string instance = normalize_instance(instance_input);
    if (instance.empty()) {
        result.error = "Empty instance";
        return result;
    }

    std::vector<std::pair<std::string, std::string>> params = {
        {"client_name", kClientName},
        {"redirect_uris", redirect_uri},
        {"scopes", kScopes},
        {"website", "https://github.com/masonasons/FastSM"}};
    net::HttpRequest req;
    req.method = "POST";
    req.url = instance + "/api/v1/apps";
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    req.body = util::form_encode(params);
    const net::HttpResponse res = http_->send(req);
    if (!res.ok()) {
        result.error = "App registration failed (" + std::to_string(res.status) + ")";
        return result;
    }
    try {
        const json j = json::parse(res.body);
        result.credentials.instance_url = instance;
        result.credentials.client_id = j.value("client_id", "");
        result.credentials.client_secret = j.value("client_secret", "");
    } catch (...) {
        result.error = "Bad app registration response";
        return result;
    }
    if (result.credentials.client_id.empty()) {
        result.error = "No client_id returned";
        return result;
    }

    result.state = random_state();
    result.authorize_url = instance + "/oauth/authorize?response_type=code&client_id=" +
                           util::percent_encode(result.credentials.client_id) +
                           "&redirect_uri=" + util::percent_encode(redirect_uri) +
                           "&scope=" + util::percent_encode(kScopes) + "&state=" + result.state;
    result.ok = true;
    return result;
}

// Portable step 2: exchange `code` for an access token, then verify credentials
// to fetch the authenticated user. `redirect_uri` must match the one used in
// begin_login().
MastodonLoginResult MastodonAuth::finish_login(const MastodonCredentials& creds,
                                               const std::string& code,
                                               const std::string& redirect_uri) {
    MastodonLoginResult result;
    result.credentials = creds;
    const std::string& instance = creds.instance_url;
    if (instance.empty() || code.empty()) {
        result.error = "Missing instance or authorization code";
        return result;
    }

    // Exchange the code for an access token.
    {
        std::vector<std::pair<std::string, std::string>> params = {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", creds.client_id},
            {"client_secret", creds.client_secret},
            {"redirect_uri", redirect_uri},
            {"scope", kScopes}};
        net::HttpRequest req;
        req.method = "POST";
        req.url = instance + "/oauth/token";
        req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        req.body = util::form_encode(params);
        const net::HttpResponse res = http_->send(req);
        if (!res.ok()) {
            result.error = "Token exchange failed (" + std::to_string(res.status) + ")";
            return result;
        }
        try {
            result.credentials.access_token = json::parse(res.body).value("access_token", "");
        } catch (...) {
            result.error = "Bad token response";
            return result;
        }
        if (result.credentials.access_token.empty()) {
            result.error = "No access token returned";
            return result;
        }
    }

    // Verify credentials -> me.
    {
        net::HttpRequest req;
        req.method = "GET";
        req.url = instance + "/api/v1/accounts/verify_credentials";
        req.headers.push_back({"Authorization", "Bearer " + result.credentials.access_token});
        const net::HttpResponse res = http_->send(req);
        if (!res.ok()) {
            result.error = "verify_credentials failed (" + std::to_string(res.status) + ")";
            return result;
        }
        try {
            result.me = mastodon::map_user(json::parse(res.body));
        } catch (...) {
            result.error = "Bad verify_credentials response";
            return result;
        }
    }

    result.ok = true;
    return result;
}

#ifdef FASTSM_HAVE_LOOPBACK_LISTENER
// Desktop interactive login: begin_login against a loopback redirect, open the
// browser, wait for the redirect, then finish_login. Blocking — worker thread.
MastodonLoginResult
MastodonAuth::login(const std::string& instance_input,
                    const std::function<void(const std::string&)>& open_browser) {
    LoopbackListener listener;
    if (!listener.start()) {
        MastodonLoginResult result;
        result.error = "Could not start local listener";
        return result;
    }
    const std::string redirect = listener.redirect_uri();

    MastodonBeginResult begun = begin_login(instance_input, redirect);
    if (!begun.ok) {
        MastodonLoginResult result;
        result.error = begun.error;
        return result;
    }

    open_browser(begun.authorize_url);

    const std::string code = listener.wait_for_code();
    if (code.empty()) {
        MastodonLoginResult result;
        result.credentials = begun.credentials;
        result.error = "No authorization code received";
        return result;
    }

    return finish_login(begun.credentials, code, redirect);
}
#else  // mobile: no loopback listener. Front ends drive begin_login/
       // finish_login around their own redirect handling.
MastodonLoginResult MastodonAuth::login(const std::string& instance_input,
                                        const std::function<void(const std::string&)>&) {
    MastodonLoginResult result;
    (void)instance_input;
    result.error = "Use begin_login/finish_login on this platform";
    return result;
}
#endif // FASTSM_HAVE_LOOPBACK_LISTENER

} // namespace fastsm
