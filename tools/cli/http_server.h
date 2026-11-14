// A very small HTTP/1.1 server for `alphaclip preview`: loopback only, one request per connection, one connection at
// a time. It exists so that the preview page can ask the native core for frames; it is not a general web server.
//
// What it guarantees:
//   - it listens on 127.0.0.1 only, never on another interface;
//   - a request whose Host header is not this server's own address is refused (403): a web page on another site
//     cannot reach it through a DNS name that resolves to 127.0.0.1;
//   - a request that carries an Origin header other than this server's own is refused (403): another site cannot
//     post to it;
//   - malformed, oversized or slow requests get an error answer or are dropped; none of them can crash it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cli {

struct HttpRequest {
  std::string method;                          // "GET", "POST", ... as sent
  std::string path;                            // percent-decoded, without the query: "/api/state"
  std::map<std::string, std::string> query;    // percent-decoded parameters
  std::map<std::string, std::string> headers;  // names in lower case
  std::vector<uint8_t> body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;                                          // any bytes
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers
};

class HttpServer {
 public:
  static constexpr size_t kMaxHeaderBytes = 16 * 1024;
  static constexpr uint64_t kMaxBodyBytes = 1ull << 30;  // 1 GiB, the largest clip the tools read

  HttpServer();
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Starts listening on 127.0.0.1:`port`; port 0 picks any free port. False (with `error`) when that fails.
  bool Listen(int port, std::string* error);
  int port() const;

  // The handler may move the body out of the request.
  using Handler = std::function<void(HttpRequest&, HttpResponse*)>;
  // Serves until Stop() is called (from the handler). An exception the handler throws becomes a 500 answer.
  void Serve(const Handler& handler);
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cli
