#include "http_server.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <list>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ac_socket;
#define AC_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int ac_socket;
#define AC_BAD_SOCKET (-1)
#endif

namespace cli {
namespace {

void CloseSocket(ac_socket s) {
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

void SetTimeouts(ac_socket s, int seconds) {
#ifdef _WIN32
  const DWORD ms = static_cast<DWORD>(seconds) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
  timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Bytes received, 0 when the peer closed, -1 on an error or a timeout.
int Receive(ac_socket s, char* buf, size_t n) {
  const int want = n > (1u << 20) ? (1 << 20) : static_cast<int>(n);
  return static_cast<int>(recv(s, buf, want, 0));
}

bool SendAll(ac_socket s, const char* data, size_t n) {
  while (n > 0) {
    const int chunk = n > (1u << 20) ? (1 << 20) : static_cast<int>(n);
#ifdef MSG_NOSIGNAL
    const int sent = static_cast<int>(send(s, data, chunk, MSG_NOSIGNAL));
#else
    const int sent = static_cast<int>(send(s, data, chunk, 0));
#endif
    if (sent <= 0) return false;
    data += sent;
    n -= static_cast<size_t>(sent);
  }
  return true;
}

const char* Reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 422: return "Unprocessable Content";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Status";
  }
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decoding. False for a broken escape or an encoded NUL.
bool UrlDecode(const std::string& in, bool plus_is_space, std::string* out) {
  out->clear();
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) return false;
      const int hi = HexValue(in[i + 1]);
      const int lo = HexValue(in[i + 2]);
      if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
      out->push_back(static_cast<char>(hi * 16 + lo));
      i += 2;
    } else if (c == '+' && plus_is_space) {
      out->push_back(' ');
    } else {
      out->push_back(c);
    }
  }
  return true;
}

std::string Trim(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The request line and the headers (`head` ends before the blank line). 0 when fine, otherwise the status to answer.
int ParseHead(const std::string& head, HttpRequest* req) {
  size_t line_end = head.find("\r\n");
  const std::string first = head.substr(0, line_end);
  const size_t sp1 = first.find(' ');
  const size_t sp2 = first.rfind(' ');
  if (sp1 == std::string::npos || sp2 == sp1) return 400;
  req->method = first.substr(0, sp1);
  const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string version = first.substr(sp2 + 1);
  if (req->method.empty() || version.compare(0, 7, "HTTP/1.") != 0) return 400;
  for (char c : req->method) {
    if (c < 'A' || c > 'Z') return 400;
  }
  if (target.empty() || target[0] != '/' || target.find(' ') != std::string::npos) return 400;

  const size_t q = target.find('?');
  if (!UrlDecode(target.substr(0, q), false, &req->path)) return 400;
  if (q != std::string::npos) {
    size_t at = q + 1;
    while (at <= target.size()) {
      size_t amp = target.find('&', at);
      if (amp == std::string::npos) amp = target.size();
      const std::string pair = target.substr(at, amp - at);
      if (!pair.empty()) {
        const size_t eq = pair.find('=');
        std::string key;
        std::string value;
        if (!UrlDecode(pair.substr(0, eq), true, &key)) return 400;
        if (eq != std::string::npos && !UrlDecode(pair.substr(eq + 1), true, &value)) return 400;
        req->query[key] = value;
      }
      at = amp + 1;
    }
  }

  while (line_end != std::string::npos) {
    const size_t start = line_end + 2;
    if (start >= head.size()) break;
    line_end = head.find("\r\n", start);
    const std::string line = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return 400;
    const std::string name = Lower(line.substr(0, colon));
    const std::string value = Trim(line.substr(colon + 1));
    if (name.find(' ') != std::string::npos || name.find('\t') != std::string::npos) return 400;
    auto it = req->headers.find(name);
    if (it != req->headers.end()) {
      // Two different lengths (or hosts) would let two parsers disagree about the request: refuse.
      if ((name == "content-length" || name == "host") && it->second != value) return 400;
      it->second += ", " + value;
    } else {
      req->headers[name] = value;
    }
  }
  return 0;
}

}  // namespace

// One accepted connection and the thread that reads its request. Browsers open connections they never use
// (preconnect); reading each on its own thread keeps such a connection from holding up the real requests. The
// handler itself still runs for one request at a time.
struct Connection {
  ac_socket socket = AC_BAD_SOCKET;
  std::thread thread;
  std::mutex mutex;  // guards `open`
  bool open = true;  // the socket has not been closed yet
  std::atomic<bool> done{false};
};

void ShutdownBoth(ac_socket s) {
#ifdef _WIN32
  shutdown(s, SD_BOTH);
#else
  shutdown(s, SHUT_RDWR);
#endif
}

struct HttpServer::Impl {
  ac_socket listener = AC_BAD_SOCKET;
  int port = 0;
  std::atomic<bool> stop{false};
  bool started = false;
  std::mutex handler_mutex;

  void Run(Connection* conn, const Handler* handler) {
    const ac_socket c = conn->socket;
    SetTimeouts(c, 15);
    HandleConnection(c, *handler);
    // Tell the peer that the answer is complete, then let it finish reading before the socket goes away.
#ifdef _WIN32
    shutdown(c, SD_SEND);
#else
    shutdown(c, SHUT_WR);
#endif
    SetTimeouts(c, 1);
    char drain[1024];
    for (int i = 0; i < 64 && Receive(c, drain, sizeof(drain)) > 0; ++i) {
    }
    {
      std::lock_guard<std::mutex> lock(conn->mutex);
      CloseSocket(c);
      conn->open = false;
    }
    conn->done = true;
  }

  void Answer(ac_socket c, const HttpResponse& res, bool head_only) {
    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " + Reason(res.status) + "\r\n";
    if (res.status != 204) head += "Content-Type: " + res.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(res.status == 204 ? 0 : res.body.size()) + "\r\n";
    head += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    for (const auto& h : res.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    if (!SendAll(c, head.data(), head.size())) return;
    if (!head_only && res.status != 204 && !res.body.empty()) SendAll(c, res.body.data(), res.body.size());
  }

  void AnswerError(ac_socket c, int status, const char* text) {
    HttpResponse res;
    res.status = status;
    res.body = text;
    res.body += "\n";
    Answer(c, res, false);
  }

  bool OwnHost(const std::string& host) const {
    const std::string p = ":" + std::to_string(port);
    return host == "127.0.0.1" + p || host == "localhost" + p;
  }

  void HandleConnection(ac_socket c, const Handler& handler) {
    // 1. the head
    std::string buf;
    size_t head_end = std::string::npos;
    char chunk[8192];
    while (head_end == std::string::npos) {
      if (buf.size() > kMaxHeaderBytes) {
        AnswerError(c, 431, "the request head is too large");
        return;
      }
      const int n = Receive(c, chunk, sizeof(chunk));
      if (n <= 0) return;  // closed, broken or too slow: nothing to answer
      const size_t from = buf.size() > 3 ? buf.size() - 3 : 0;
      buf.append(chunk, static_cast<size_t>(n));
      head_end = buf.find("\r\n\r\n", from);
    }
    if (head_end > kMaxHeaderBytes) {
      AnswerError(c, 431, "the request head is too large");
      return;
    }
    HttpRequest req;
    const int bad = ParseHead(buf.substr(0, head_end), &req);
    if (bad != 0) {
      AnswerError(c, bad, "malformed request");
      return;
    }

    // 2. who is asking
    auto host = req.headers.find("host");
    if (host == req.headers.end() || !OwnHost(host->second)) {
      AnswerError(c, 403, "this server only answers to its own address");
      return;
    }
    auto origin = req.headers.find("origin");
    if (origin != req.headers.end() && !(origin->second.compare(0, 7, "http://") == 0 && OwnHost(origin->second.substr(7)))) {
      AnswerError(c, 403, "requests from other origins are refused");
      return;
    }

    // 3. the body
    if (req.headers.count("transfer-encoding") != 0) {
      AnswerError(c, 501, "chunked requests are not supported; send Content-Length");
      return;
    }
    uint64_t length = 0;
    auto cl = req.headers.find("content-length");
    if (cl != req.headers.end()) {
      if (cl->second.empty() || cl->second.size() > 12) {
        AnswerError(c, cl->second.empty() ? 400 : 413, "bad Content-Length");
        return;
      }
      for (char d : cl->second) {
        if (d < '0' || d > '9') {
          AnswerError(c, 400, "bad Content-Length");
          return;
        }
        length = length * 10 + static_cast<uint64_t>(d - '0');
      }
    }  // without the header the request has no body (RFC 9112, 6.3)
    if (length > kMaxBodyBytes) {
      AnswerError(c, 413, "the request body is too large");
      return;
    }
    const size_t body_start = head_end + 4;
    if (buf.size() - body_start > length) {
      AnswerError(c, 400, "more bytes than Content-Length announced");
      return;
    }
    auto expect = req.headers.find("expect");
    if (expect != req.headers.end() && Lower(expect->second) == "100-continue") {
      const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
      if (!SendAll(c, go, sizeof(go) - 1)) return;
    }
    try {
      req.body.reserve(static_cast<size_t>(length));
    } catch (const std::exception&) {
      AnswerError(c, 413, "not enough memory for the request body");
      return;
    }
    req.body.assign(buf.begin() + static_cast<std::ptrdiff_t>(body_start), buf.end());
    buf.clear();
    buf.shrink_to_fit();
    while (req.body.size() < length) {
      const uint64_t left = length - req.body.size();
      const int n = Receive(c, chunk, left < sizeof(chunk) ? static_cast<size_t>(left) : sizeof(chunk));
      if (n <= 0) return;  // the body never arrived completely
      req.body.insert(req.body.end(), chunk, chunk + n);
    }

    // 4. the answer
    HttpResponse res;
    try {
      std::lock_guard<std::mutex> lock(handler_mutex);
      handler(req, &res);
    } catch (const std::exception& e) {
      res = HttpResponse();
      res.status = 500;
      res.body = std::string("internal error: ") + e.what() + "\n";
    } catch (...) {
      res = HttpResponse();
      res.status = 500;
      res.body = "internal error\n";
    }
    Answer(c, res, req.method == "HEAD");
  }
};

HttpServer::HttpServer() : impl_(new Impl()) {}

HttpServer::~HttpServer() {
  if (impl_->listener != AC_BAD_SOCKET) CloseSocket(impl_->listener);
#ifdef _WIN32
  if (impl_->started) WSACleanup();
#endif
}

int HttpServer::port() const { return impl_->port; }

void HttpServer::Stop() { impl_->stop = true; }

bool HttpServer::Listen(int port, std::string* error) {
  if (port < 0 || port > 65535) {
    *error = "the port must be 0..65535";
    return false;
  }
#ifdef _WIN32
  if (!impl_->started) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      *error = "cannot start Windows sockets";
      return false;
    }
    impl_->started = true;
  }
#endif
  if (impl_->listener != AC_BAD_SOCKET) {
    CloseSocket(impl_->listener);
    impl_->listener = AC_BAD_SOCKET;
  }
  const ac_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == AC_BAD_SOCKET) {
    *error = "cannot create a socket";
    return false;
  }
  int one = 1;
#ifdef _WIN32
  // Nobody else may bind the same port while this server runs.
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only: never reachable from another machine
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
    CloseSocket(s);
    *error = "cannot listen on 127.0.0.1:" + std::to_string(port) + " (is the port in use?)";
    return false;
  }
  sockaddr_in bound;
  std::memset(&bound, 0, sizeof(bound));
#ifdef _WIN32
  int len = sizeof(bound);
#else
  socklen_t len = sizeof(bound);
#endif
  if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
    CloseSocket(s);
    *error = "cannot read the port of the socket";
    return false;
  }
  impl_->listener = s;
  impl_->port = ntohs(bound.sin_port);
  return true;
}

void HttpServer::Serve(const Handler& handler) {
  constexpr size_t kMaxConnections = 64;
  std::list<std::unique_ptr<Connection>> connections;
  // Joins the connections that are finished; with `all`, cuts the others short first and joins them too.
  auto reap = [&connections](bool all) {
    for (auto it = connections.begin(); it != connections.end();) {
      Connection* conn = it->get();
      if (all) {
        std::lock_guard<std::mutex> lock(conn->mutex);
        if (conn->open) ShutdownBoth(conn->socket);
      }
      if (all || conn->done) {
        conn->thread.join();
        it = connections.erase(it);
      } else {
        ++it;
      }
    }
  };

  impl_->stop = false;
  while (!impl_->stop && impl_->listener != AC_BAD_SOCKET) {
    reap(false);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(impl_->listener, &readable);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
#ifdef _WIN32
    const int ready = select(0, &readable, nullptr, nullptr, &tv);
#else
    const int ready = select(impl_->listener + 1, &readable, nullptr, nullptr, &tv);
#endif
    if (ready <= 0) continue;
    const ac_socket c = accept(impl_->listener, nullptr, nullptr);
    if (c == AC_BAD_SOCKET) continue;
    if (connections.size() >= kMaxConnections) {
      CloseSocket(c);  // far more than a browser opens: refuse rather than grow without bound
      continue;
    }
    connections.emplace_back(new Connection());
    Connection* conn = connections.back().get();
    conn->socket = c;
    Impl* impl = impl_.get();
    const Handler* h = &handler;
    conn->thread = std::thread([impl, conn, h] { impl->Run(conn, h); });
  }
  reap(true);
}

}  // namespace cli
