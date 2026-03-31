#include "http_api_server.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include <spdlog/spdlog.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logging_utils.h"

namespace video_server
{
    namespace
    {

        std::string trim_cr(std::string value)
        {
            if (!value.empty() && value.back() == '\r')
            {
                value.pop_back();
            }
            return value;
        }

        std::string lowercase_ascii(std::string value)
        {
            for (char &c : value)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return value;
        }

        std::string reason_phrase(int status)
        {
            switch (status)
            {
            case 200:
                return "OK";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 413:
                return "Payload Too Large";
            case 429:
                return "Too Many Requests";
            case 500:
                return "Internal Server Error";
            default:
                return "OK";
            }
        }

        bool parse_request(const std::string &raw, HttpRequest &out_request)
        {
            const auto header_end = raw.find("\r\n\r\n");
            if (header_end == std::string::npos)
            {
                return false;
            }

            std::istringstream headers(raw.substr(0, header_end));
            std::string request_line;
            if (!std::getline(headers, request_line))
            {
                return false;
            }
            request_line = trim_cr(request_line);

            std::istringstream req_line_stream(request_line);
            std::string target;
            std::string version;
            if (!(req_line_stream >> out_request.method >> target >> version))
            {
                return false;
            }

            const auto query_pos = target.find('?');
            out_request.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);

            size_t content_length = 0;
            for (std::string line; std::getline(headers, line);)
            {
                line = trim_cr(line);
                if (line.empty())
                {
                    continue;
                }
                const auto colon = line.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                {
                    value.erase(value.begin());
                }
                if (key == "Content-Length")
                {
                    content_length = static_cast<size_t>(std::stoul(value));
                }
                out_request.headers.emplace(lowercase_ascii(std::move(key)), std::move(value));
            }

            const size_t body_start = header_end + 4;
            if (raw.size() < body_start + content_length)
            {
                return false;
            }
            out_request.body = raw.substr(body_start, content_length);
            return true;
        }

        std::string response_to_http(const HttpResponse &response)
        {
            const std::string content_type = response.content_type.empty() ? "application/json" : response.content_type;
            std::ostringstream out;
            out << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n";
            out << "Content-Type: " << content_type << "\r\n";
            for (const auto &[key, value] : response.headers)
            {
                out << key << ": " << value << "\r\n";
            }
            out << "Content-Length: " << response.body.size() << "\r\n";
            out << "Connection: close\r\n\r\n";
            out << response.body;
            return out.str();
        }

    } // namespace

    HttpApiServer::HttpApiServer(std::string host, uint16_t port, size_t max_request_bytes)
        : host_(std::move(host)), port_(port), max_request_bytes_(max_request_bytes) {}
    HttpApiServer::~HttpApiServer() { stop(); }

    bool HttpApiServer::open(Handler handler)
    {
        ensure_default_logging_config();
        if (running_ || listen_fd_ >= 0)
        {
            return false;
        }

        handler_ = std::move(handler);

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        const std::string port_str = std::to_string(static_cast<unsigned int>(port_));
        addrinfo *result = nullptr;
        if (::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0)
        {
            return false;
        }

        int bound_fd = -1;
        for (addrinfo *ai = result; ai != nullptr; ai = ai->ai_next)
        {
            const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
            {
                continue;
            }

            int reuse = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 16) == 0)
            {
                bound_fd = fd;
                break;
            }

            ::close(fd);
        }

        ::freeaddrinfo(result);

        if (bound_fd < 0)
        {
            return false;
        }

        listen_fd_ = bound_fd;
        return true;
    }

    bool HttpApiServer::start(Handler handler, int poll_timeout_ms)
    {
        if (!open(std::move(handler)))
        {
            return false;
        }

        running_ = true;
        server_thread_ = std::thread([this, poll_timeout_ms]()
                                     { run_loop(poll_timeout_ms); });
        return true;
    }

    bool HttpApiServer::pump_once(int timeout_ms, size_t max_connections)
    {
        if (listen_fd_ >= 0)
        {
            const int local_listen_fd = listen_fd_;
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(local_listen_fd, &read_set);

            timeval timeout{};
            if (timeout_ms > 0)
            {
                timeout.tv_sec = timeout_ms / 1000;
                timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
            }

            const int ready = ::select(local_listen_fd + 1, &read_set, nullptr, nullptr, timeout_ms >= 0 ? &timeout : nullptr);
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    return true;
                }
                return false;
            }
            if (ready == 0)
            {
                return true;
            }

            size_t accepted = 0;
            while (accepted < max_connections)
            {
                sockaddr_storage client_addr{};
                socklen_t client_addr_len = sizeof(client_addr);
                const int client_fd = ::accept(local_listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_addr_len);
                if (client_fd < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        return true;
                    }
                    return false;
                }

                char host_buffer[NI_MAXHOST] = {};
                std::string remote_address = "unknown";
                if (::getnameinfo(reinterpret_cast<const sockaddr *>(&client_addr), client_addr_len, host_buffer,
                                  sizeof(host_buffer), nullptr, 0, NI_NUMERICHOST) == 0)
                {
                    remote_address = host_buffer;
                }

                handle_client(client_fd, remote_address);
                ::close(client_fd);
                ++accepted;

                timeval immediate_timeout{};
                fd_set immediate_read_set;
                FD_ZERO(&immediate_read_set);
                FD_SET(local_listen_fd, &immediate_read_set);
                const int more_ready = ::select(local_listen_fd + 1, &immediate_read_set, nullptr, nullptr, &immediate_timeout);
                if (more_ready <= 0)
                {
                    break;
                }
            }
        }

        return true;
    }

    bool HttpApiServer::is_open() const { return listen_fd_ >= 0; }

    void HttpApiServer::close()
    {
        if (listen_fd_ >= 0)
        {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    void HttpApiServer::stop()
    {
        running_ = false;
        close();
        if (server_thread_.joinable())
        {
            server_thread_.join();
        }
    }

    void HttpApiServer::run_loop(int poll_timeout_ms)
    {
        while (running_)
        {
            if (!pump_once(poll_timeout_ms))
            {
                break;
            }
        }
    }

    void HttpApiServer::handle_client(int client_fd, const std::string &remote_address) const
    {
        std::string raw;
        raw.reserve(4096);
        std::array<char, 1024> buf{};
        bool request_too_large = false;

        size_t expected_total = 0;
        while (true)
        {
            const ssize_t n = ::recv(client_fd, buf.data(), buf.size(), 0);
            if (n <= 0)
            {
                break;
            }
            raw.append(buf.data(), static_cast<size_t>(n));
            if (raw.size() > max_request_bytes_)
            {
                request_too_large = true;
                break;
            }

            const auto header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos && expected_total == 0)
            {
                expected_total = header_end + 4;
                const auto cl = raw.find("Content-Length:");
                if (cl != std::string::npos && cl < header_end)
                {
                    const auto line_end = raw.find("\r\n", cl);
                    const auto value_start = cl + std::strlen("Content-Length:");
                    if (line_end != std::string::npos)
                    {
                        std::string value = raw.substr(value_start, line_end - value_start);
                        while (!value.empty() && value.front() == ' ')
                        {
                            value.erase(value.begin());
                        }
                        expected_total += static_cast<size_t>(std::stoul(value));
                        if (expected_total > max_request_bytes_)
                        {
                            request_too_large = true;
                            break;
                        }
                    }
                }
            }

            if (expected_total > 0 && raw.size() >= expected_total)
            {
                break;
            }
        }

        HttpRequest request;
        HttpResponse response;
        if (request_too_large)
        {
            response = HttpResponse{413, "{\"error\":\"request too large\"}", "application/json"};
        }
        else if (!parse_request(raw, request))
        {
            spdlog::warn("[http] failed to parse incoming request");
            response = HttpResponse{400, "{\"error\":\"invalid request\"}", "application/json"};
        }
        else if (!handler_)
        {
            response = HttpResponse{500, "{\"error\":\"handler unavailable\"}", "application/json"};
        }
        else
        {
            request.remote_address = remote_address;
            response = handler_(request);
        }

        if (response.content_type.empty())
        {
            response.content_type = "application/json";
        }

        const std::string http = response_to_http(response);
        size_t sent = 0;
        while (sent < http.size())
        {
            const ssize_t n = ::send(client_fd, http.data() + sent, http.size() - sent, 0);
            if (n <= 0)
            {
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }

} // namespace video_server
