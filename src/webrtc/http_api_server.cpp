#include "http_api_server.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace video_server {
namespace {

std::string trim_cr(std::string value) {
  if (!value.empty() && value.back() == '\r') {
    value.pop_back();
  }
  return value;
}

std::string reason_phrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 500:
      return "Internal Server Error";
    default:
      return "OK";
  }
}

bool parse_request(const std::string& raw, HttpRequest& out_request) {
  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return false;
  }

  std::istringstream headers(raw.substr(0, header_end));
  std::string request_line;
  if (!std::getline(headers, request_line)) {
    return false;
  }
  request_line = trim_cr(request_line);

  std::istringstream req_line_stream(request_line);
  std::string target;
  std::string version;
  if (!(req_line_stream >> out_request.method >> target >> version)) {
    return false;
  }

  const auto query_pos = target.find('?');
  out_request.path = query_pos == std::string::npos ? target : target.substr(0, query_pos);

  size_t content_length = 0;
  for (std::string line; std::getline(headers, line);) {
    line = trim_cr(line);
    if (line.empty()) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    if (key == "Content-Length") {
      content_length = static_cast<size_t>(std::stoul(value));
    }
  }

  const size_t body_start = header_end + 4;
  if (raw.size() < body_start + content_length) {
    return false;
  }
  out_request.body = raw.substr(body_start, content_length);
  return true;
}

std::string response_to_http(const HttpResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n";
  out << "Content-Type: " << response.content_type << "\r\n";
  out << "Content-Length: " << response.body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << response.body;
  return out.str();
}

}  // namespace

HttpApiServer::HttpApiServer(uint16_t port) : port_(port) {}
HttpApiServer::~HttpApiServer() { stop(); }

bool HttpApiServer::start(Handler handler) {
  if (running_) {
    return false;
  }

  handler_ = std::move(handler);

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return false;
  }

  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (::listen(listen_fd_, 16) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_ = true;
  server_thread_ = std::thread([this]() { run_loop(); });
  return true;
}

void HttpApiServer::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  if (server_thread_.joinable()) {
    server_thread_.join();
  }
}

void HttpApiServer::run_loop() {
  while (running_) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listen_fd_, &read_set);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;

    const int ready = ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &timeout);
    if (!running_) {
      break;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      continue;
    }

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    const int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      continue;
    }

    handle_client(client_fd);
    ::close(client_fd);
  }
}

void HttpApiServer::handle_client(int client_fd) const {
  std::string raw;
  raw.reserve(4096);
  std::array<char, 1024> buf{};

  size_t expected_total = 0;
  while (true) {
    const ssize_t n = ::recv(client_fd, buf.data(), buf.size(), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buf.data(), static_cast<size_t>(n));

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos && expected_total == 0) {
      expected_total = header_end + 4;
      const auto cl = raw.find("Content-Length:");
      if (cl != std::string::npos && cl < header_end) {
        const auto line_end = raw.find("\r\n", cl);
        const auto value_start = cl + std::strlen("Content-Length:");
        if (line_end != std::string::npos) {
          std::string value = raw.substr(value_start, line_end - value_start);
          while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
          }
          expected_total += static_cast<size_t>(std::stoul(value));
        }
      }
    }

    if (expected_total > 0 && raw.size() >= expected_total) {
      break;
    }
  }

  HttpRequest request;
  HttpResponse response;
  if (!parse_request(raw, request)) {
    response = HttpResponse{400, "{\"error\":\"invalid request\"}"};
  } else if (!handler_) {
    response = HttpResponse{500, "{\"error\":\"handler unavailable\"}"};
  } else {
    response = handler_(request);
  }

  const std::string http = response_to_http(response);
  size_t sent = 0;
  while (sent < http.size()) {
    const ssize_t n = ::send(client_fd, http.data() + sent, http.size() - sent, 0);
    if (n <= 0) {
      break;
    }
    sent += static_cast<size_t>(n);
  }
}

}  // namespace video_server
