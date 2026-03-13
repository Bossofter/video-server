#include "http_api_server.h"

namespace video_server {

HttpApiServer::HttpApiServer(uint16_t port) : port_(port) {}
HttpApiServer::~HttpApiServer() { stop(); }

bool HttpApiServer::start(Handler handler) {
  (void)port_;
  (void)handler;
  running_ = true;
  return true;
}

void HttpApiServer::stop() { running_ = false; }

}  // namespace video_server
