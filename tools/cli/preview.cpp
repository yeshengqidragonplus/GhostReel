// `alphaclip preview [<clip.webm>] [--meta <clip.acmeta.json> | --no-meta] [--port N] [--no-open]` (design doc TR-4).
//
// Starts a small web server on 127.0.0.1 and opens the preview page in the browser. The page shows the clip the way
// an engine binding does: this process decodes with the native core (the same code the engines load) and hands out
// YUV(A) planes; the page uploads them as textures and draws them with the master shader (shaders/). Files dropped on
// the page are sent here to be decoded; nothing is ever written to disk.
//
//   GET  /                               the page (embedded in the executable; see preview_assets.h)
//   GET  /api/state                      {"version", "clip": the most recently used clip or null}
//   POST /api/clips?name=<file name>     body: the bytes of a .webm      -> {"id", "name", "size"}; not opened yet
//   POST /api/clips/<id>/open            body: the sidecar text, or empty for none ("auto")
//                                        -> the clip {"id","name","size","meta","info","inspect","error"}
//                                        or 422 {"error","status"} when the core refuses; the clip then stays as it was
//   GET  /api/clips/<id>/frame?at_us=T   the frame shown at time T (ac_seek + ac_decode_next)
//   GET  /api/clips/<id>/frame?after_us=P  the frame that follows the one with pts P; 204 at the end of the clip
//                                        -> 32-byte header ("ACF1", width, height, flags, pts_us, reserved; little
//                                        endian) + the Y, U, V[, A] planes, tightly packed
//   POST /api/quit                       stops the server (used by the tests)
//
// --port N      listen on that port (0: any free one). Default: 47800, or any free port when that one is taken.
// --no-open     do not start the browser; the address is printed as "url: http://127.0.0.1:N/".
// --dev-root D  read the page's files from the repository at D instead of the embedded copies (for working on the page).
// Up to four clips are kept in memory; the least recently used one makes room for a new one.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "alphaclip.h"
#include "commands.h"
#include "http_server.h"
#include "preview_assets.h"
#include "util.h"

namespace cli {
namespace {

constexpr int kDefaultPort = 47800;
constexpr size_t kMaxSessions = 4;
constexpr size_t kMaxSidecarBytes = 1u << 20;  // the core itself stops at 64 KiB; this only bounds the request

const char* StatusName(ac_status s) {
  switch (s) {
    case AC_OK: return "AC_OK";
    case AC_AGAIN: return "AC_AGAIN";
    case AC_ERR_IO: return "AC_ERR_IO";
    case AC_ERR_FORMAT: return "AC_ERR_FORMAT";
    case AC_ERR_UNSUPPORTED: return "AC_ERR_UNSUPPORTED";
    case AC_ERR_DECODE: return "AC_ERR_DECODE";
    case AC_ERR_STATE: return "AC_ERR_STATE";
    case AC_ERR_OOM: return "AC_ERR_OOM";
    case AC_ERR_ARG: return "AC_ERR_ARG";
    case AC_END: return "AC_END";
  }
  return "AC_ERR";
}

const char* ModeName(int mode) {
  switch (mode) {
    case AC_ALPHA_OPAQUE: return "opaque";
    case AC_ALPHA_PACKED_RIGHT: return "packed_right";
    case AC_ALPHA_PACKED_BOTTOM: return "packed_bottom";
    case AC_ALPHA_NATIVE: return "native_alpha";
    default: return "unknown";
  }
}

// Length of the well-formed UTF-8 sequence at p (1..4), or 0.
size_t Utf8Length(const unsigned char* p, size_t left) {
  auto cont = [&](size_t i) { return i < left && (p[i] & 0xC0) == 0x80; };
  if (p[0] < 0x80) return 1;
  if (p[0] >= 0xC2 && p[0] <= 0xDF) return cont(1) ? 2 : 0;
  if (p[0] >= 0xE0 && p[0] <= 0xEF) {
    if (!cont(1) || !cont(2)) return 0;
    if (p[0] == 0xE0 && p[1] < 0xA0) return 0;   // overlong
    if (p[0] == 0xED && p[1] >= 0xA0) return 0;  // surrogates
    return 3;
  }
  if (p[0] >= 0xF0 && p[0] <= 0xF4) {
    if (!cont(1) || !cont(2) || !cont(3)) return 0;
    if (p[0] == 0xF0 && p[1] < 0x90) return 0;   // overlong
    if (p[0] == 0xF4 && p[1] >= 0x90) return 0;  // above U+10FFFF
    return 4;
  }
  return 0;
}

// A JSON string literal. Bytes that are not UTF-8 become U+FFFD, so the output is always valid JSON.
std::string JsonString(const std::string& s) {
  std::string out = "\"";
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = p[i];
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
      ++i;
    } else if (c == '\n') {
      out += "\\n";
      ++i;
    } else if (c < 0x20 || c == 0x7F) {
      char esc[8];
      std::snprintf(esc, sizeof(esc), "\\u%04x", c);
      out += esc;
      ++i;
    } else {
      const size_t n = Utf8Length(p + i, s.size() - i);
      if (n == 0) {
        out += "\\ufffd";
        ++i;
      } else {
        out.append(s, i, n);
        i += n;
      }
    }
  }
  out.push_back('"');
  return out;
}

void JsonError(HttpResponse* res, int http_status, const std::string& message, const char* status_name = "") {
  res->status = http_status;
  res->content_type = "application/json; charset=utf-8";
  res->body = "{\"error\":" + JsonString(message) + ",\"status\":" + JsonString(status_name) + "}\n";
}

struct Session {
  int64_t id = 0;
  std::string name;
  std::vector<uint8_t> bytes;
  std::string meta;      // the sidecar the clip is open with (or, while the core refuses the clip, the one it refused)
  bool has_meta = false;
  ac_player* player = nullptr;  // null while the core refuses the clip
  ac_info info;
  std::string inspect;
  std::string error;     // why there is no player
  // The decoder stands right behind the frame with this pts; `last_frame` is that frame as it was sent.
  bool have_last = false;
  int64_t last_pts = 0;
  std::string last_frame;
  uint64_t used = 0;

  Session() { std::memset(&info, 0, sizeof(info)); }
  ~Session() { ac_close(player); }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

void PutU32(std::string* out, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*out)[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

void CopyPlane(char* dst, const uint8_t* src, int stride, int w, int h) {
  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(w), src + static_cast<ptrdiff_t>(y) * stride,
                static_cast<size_t>(w));
  }
}

void EncodeFrame(const ac_frame& f, std::string* out) {
  const size_t w = static_cast<size_t>(f.width);
  const size_t h = static_cast<size_t>(f.height);
  const size_t cw = (w + 1) / 2;
  const size_t ch = (h + 1) / 2;
  const bool alpha = f.plane[3] != nullptr;
  out->assign(32 + w * h + 2 * cw * ch + (alpha ? w * h : 0), '\0');
  std::memcpy(&(*out)[0], "ACF1", 4);
  PutU32(out, 4, static_cast<uint32_t>(f.width));
  PutU32(out, 8, static_cast<uint32_t>(f.height));
  PutU32(out, 12, alpha ? 1u : 0u);
  const uint64_t pts = static_cast<uint64_t>(f.pts_us);
  PutU32(out, 16, static_cast<uint32_t>(pts & 0xFFFFFFFFu));
  PutU32(out, 20, static_cast<uint32_t>(pts >> 32));
  char* at = &(*out)[32];
  CopyPlane(at, f.plane[0], f.stride[0], f.width, f.height);
  at += w * h;
  CopyPlane(at, f.plane[1], f.stride[1], static_cast<int>(cw), static_cast<int>(ch));
  at += cw * ch;
  CopyPlane(at, f.plane[2], f.stride[2], static_cast<int>(cw), static_cast<int>(ch));
  at += cw * ch;
  if (alpha) CopyPlane(at, f.plane[3], f.stride[3], f.width, f.height);
}

class PreviewService {
 public:
  PreviewService(HttpServer* server, std::string dev_root) : server_(server), dev_root_(std::move(dev_root)) {}

  // A new clip in memory. It is not opened yet.
  Session* Add(std::string name, std::vector<uint8_t> bytes) {
    if (sessions_.size() >= kMaxSessions) {
      auto oldest = std::min_element(sessions_.begin(), sessions_.end(),
                                     [](const std::unique_ptr<Session>& a, const std::unique_ptr<Session>& b) {
                                       return a->used < b->used;
                                     });
      sessions_.erase(oldest);
    }
    sessions_.emplace_back(new Session());
    Session* s = sessions_.back().get();
    s->id = next_id_++;
    s->name = std::move(name);
    s->bytes = std::move(bytes);
    s->used = ++clock_;
    return s;
  }

  // Opens (or re-opens) a clip with a sidecar. False when the core refuses: a clip that was open stays open as it was.
  bool Open(Session* s, const std::string& meta, bool has_meta, const std::string& meta_desc, std::string* error,
            ac_status* status) {
    ac_player* p = nullptr;
    ac_status st = ac_open_memory(s->bytes.data(), s->bytes.size(), has_meta ? meta.c_str() : nullptr, &p);
    ac_info info;
    std::memset(&info, 0, sizeof(info));
    if (st == AC_OK) {
      st = ac_get_info(p, &info);
      if (st != AC_OK) {
        *error = ac_last_error(p);
        ac_close(p);
        p = nullptr;
      }
    } else {
      *error = ac_last_error(nullptr);
    }
    if (st != AC_OK) {
      *status = st;
      if (s->player == nullptr) {
        s->meta = meta;
        s->has_meta = has_meta;
        s->error = *error;
        Inspect(s, meta_desc);
      }
      return false;
    }
    ac_close(s->player);
    s->player = p;
    s->info = info;
    s->meta = meta;
    s->has_meta = has_meta;
    s->error.clear();
    s->have_last = false;
    s->last_frame.clear();
    Inspect(s, meta_desc);
    return true;
  }

  void Handle(HttpRequest& req, HttpResponse* res) {
    if (req.path.compare(0, 5, "/api/") == 0) {
      // A page of another site must not even trigger a request here (the browser says where a request comes from).
      auto site = req.headers.find("sec-fetch-site");
      if (site != req.headers.end() && site->second != "same-origin" && site->second != "none") {
        JsonError(res, 403, "requests from other sites are refused");
        return;
      }
      Api(req, res);
      return;
    }
    Static(req, res);
  }

 private:
  void Inspect(Session* s, const std::string& meta_desc) {
    std::string report;
    if (InspectReport(s->bytes, s->name, s->has_meta ? s->meta : std::string(), meta_desc, &report)) {
      s->inspect = report;
    } else {
      s->inspect = "error: " + report + "\n";
    }
  }

  Session* Find(int64_t id) {
    for (auto& s : sessions_) {
      if (s->id == id) {
        s->used = ++clock_;
        return s.get();
      }
    }
    return nullptr;
  }

  static std::string InfoJson(const ac_info& i) {
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "{\"width\":%d,\"height\":%d,\"content_w\":%d,\"content_h\":%d,\"duration_us\":%lld,\"fps\":%.6f,"
                  "\"has_alpha_plane\":%d,\"packet_count\":%lld,\"loop_period_us\":%lld,\"alpha_mode\":%d,"
                  "\"alpha_mode_name\":\"%s\",\"premultiplied\":%d,\"color_range\":%d,\"matrix\":%d,\"loop_hint\":%d,"
                  "\"alpha_x\":%d,\"alpha_y\":%d}",
                  i.width, i.height, i.content_w, i.content_h, static_cast<long long>(i.duration_us),
                  static_cast<double>(i.fps), i.has_alpha_plane, static_cast<long long>(i.packet_count),
                  static_cast<long long>(i.loop_period_us), i.alpha_mode, ModeName(i.alpha_mode), i.premultiplied,
                  i.color_range, i.matrix, i.loop_hint, i.alpha_x, i.alpha_y);
    return buf;
  }

  static std::string ClipJson(const Session& s) {
    std::string out = "{\"id\":" + std::to_string(s.id) + ",\"name\":" + JsonString(s.name) +
                      ",\"size\":" + std::to_string(s.bytes.size());
    out += ",\"meta\":" + (s.has_meta ? JsonString(s.meta) : std::string("null"));
    out += ",\"info\":" + (s.player != nullptr ? InfoJson(s.info) : std::string("null"));
    out += ",\"inspect\":" + JsonString(s.inspect);
    out += ",\"error\":" + (s.player != nullptr ? std::string("null") : JsonString(s.error)) + "}";
    return out;
  }

  static void Json(HttpResponse* res, const std::string& body) {
    res->content_type = "application/json; charset=utf-8";
    res->body = body + "\n";
  }

  void Api(HttpRequest& req, HttpResponse* res) {
    const std::string& path = req.path;
    if (path == "/api/state") {
      if (req.method != "GET") return JsonError(res, 405, "use GET");
      const Session* current = nullptr;
      for (const auto& s : sessions_) {
        if (current == nullptr || s->used > current->used) current = s.get();
      }
      Json(res, std::string("{\"version\":") + JsonString(ac_version()) + ",\"clip\":" +
                    (current != nullptr ? ClipJson(*current) : std::string("null")) + "}");
      return;
    }
    if (path == "/api/quit") {
      if (req.method != "POST") return JsonError(res, 405, "use POST");
      server_->Stop();
      Json(res, "{\"ok\":true}");
      return;
    }
    if (path == "/api/clips") {
      if (req.method != "POST") return JsonError(res, 405, "use POST");
      if (req.body.empty()) return JsonError(res, 400, "the request has no clip in it");
      std::string name = FileNameOf(req.query.count("name") != 0 ? req.query["name"] : std::string());
      if (name.empty()) name = "clip.webm";
      if (name.size() > 255) name.resize(255);
      const Session* s = Add(name, std::move(req.body));
      Json(res, "{\"id\":" + std::to_string(s->id) + ",\"name\":" + JsonString(s->name) + ",\"size\":" +
                    std::to_string(s->bytes.size()) + "}");
      return;
    }
    // /api/clips/<id>/open and /api/clips/<id>/frame
    const std::string prefix = "/api/clips/";
    if (path.compare(0, prefix.size(), prefix) == 0) {
      const size_t slash = path.find('/', prefix.size());
      int64_t id = 0;
      if (slash == std::string::npos || !ParseInt64(path.substr(prefix.size(), slash - prefix.size()).c_str(), &id)) {
        return JsonError(res, 404, "no such address");
      }
      const std::string action = path.substr(slash + 1);
      if (action != "open" && action != "frame") return JsonError(res, 404, "no such address");
      Session* s = Find(id);
      if (s == nullptr) return JsonError(res, 404, "this clip is no longer held by the preview; load it again");
      if (action == "open") {
        if (req.method != "POST") return JsonError(res, 405, "use POST");
        if (req.body.size() > kMaxSidecarBytes) return JsonError(res, 413, "the sidecar is too large");
        const std::string meta(req.body.begin(), req.body.end());
        std::string error;
        ac_status status = AC_OK;
        const bool has_meta = !meta.empty();
        if (!Open(s, meta, has_meta, has_meta ? "set in the preview page" : "none (auto mode)", &error, &status)) {
          return JsonError(res, 422, error, StatusName(status));
        }
        Json(res, ClipJson(*s));
        return;
      }
      if (req.method != "GET") return JsonError(res, 405, "use GET");
      Frame(s, req, res);
      return;
    }
    JsonError(res, 404, "no such address");
  }

  void Frame(Session* s, const HttpRequest& req, HttpResponse* res) {
    if (s->player == nullptr) return JsonError(res, 409, "the clip is not open: " + s->error);
    const auto at = req.query.find("at_us");
    const auto after = req.query.find("after_us");
    if ((at == req.query.end()) == (after == req.query.end())) {
      return JsonError(res, 400, "give exactly one of at_us and after_us");
    }
    int64_t t = 0;
    if (!ParseInt64((at != req.query.end() ? at : after)->second.c_str(), &t)) {
      return JsonError(res, 400, "the time must be an integer number of microseconds");
    }
    ac_player* p = s->player;
    ac_frame f;
    std::memset(&f, 0, sizeof(f));
    ac_status st = AC_OK;
    if (at != req.query.end()) {
      if (s->have_last && s->last_pts == t) {  // the frame that was sent last: no need to decode it again
        res->content_type = "application/octet-stream";
        res->body = s->last_frame;
        return;
      }
      st = ac_seek(p, t);
      if (st == AC_OK) st = ac_decode_next(p, &f);
    } else {
      bool have = false;
      if (!(s->have_last && s->last_pts == t)) {
        // Not where the decoder stands: go to the frame with that pts first.
        st = ac_seek(p, t);
        if (st == AC_OK) st = ac_decode_next(p, &f);
        have = st == AC_OK && f.pts_us > t;  // t lies before the first frame, so that frame is the one after t
      }
      if (st == AC_OK && !have) st = ac_decode_next(p, &f);
    }
    if (st == AC_END) {
      res->status = 204;
      return;
    }
    if (st != AC_OK) {
      s->have_last = false;
      return JsonError(res, 500, ac_last_error(p), StatusName(st));
    }
    EncodeFrame(f, &s->last_frame);
    s->have_last = true;
    s->last_pts = f.pts_us;
    res->content_type = "application/octet-stream";
    res->body = s->last_frame;
  }

  void Static(const HttpRequest& req, HttpResponse* res) {
    if (req.method != "GET" && req.method != "HEAD") {
      res->status = 405;
      res->body = "use GET\n";
      return;
    }
    const std::string url = req.path == "/" ? "/index.html" : req.path;
    for (size_t i = 0; i < kPreviewAssetCount; ++i) {
      const PreviewAsset& a = kPreviewAssets[i];
      if (url != a.url) continue;
      res->content_type = std::string(a.content_type) + "; charset=utf-8";
      if (std::strcmp(a.content_type, "text/html") == 0) {
        // The page loads nothing from anywhere else; the test page may talk to a result collector on this machine.
        res->headers.emplace_back("Content-Security-Policy",
                                  "default-src 'self'; connect-src 'self' http://127.0.0.1:*; img-src 'self' data: blob:; "
                                  "style-src 'self' 'unsafe-inline'; frame-ancestors 'self'");
      }
      if (!dev_root_.empty()) {
        std::vector<uint8_t> data;
        std::string error;
        if (!ReadFile(dev_root_ + "/" + a.source, &data, &error)) {
          res->status = 500;
          res->content_type = "text/plain; charset=utf-8";
          res->body = error + "\n";
          return;
        }
        res->body.assign(data.begin(), data.end());
      } else {
        res->body.assign(reinterpret_cast<const char*>(a.data), a.size);
      }
      return;
    }
    res->status = 404;
    res->body = "not found\n";
  }

  HttpServer* server_;
  std::string dev_root_;
  std::vector<std::unique_ptr<Session>> sessions_;
  int64_t next_id_ = 1;
  uint64_t clock_ = 0;
};

}  // namespace

int CmdPreview(int argc, char** argv) {
  std::string clip_path;
  std::string meta_path;
  std::string dev_root;
  bool no_meta = false;
  bool no_open = false;
  bool port_given = false;
  int64_t port = kDefaultPort;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
      meta_path = argv[++i];
    } else if (std::strcmp(argv[i], "--no-meta") == 0) {
      no_meta = true;
    } else if (std::strcmp(argv[i], "--no-open") == 0) {
      no_open = true;
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (!ParseInt64(argv[++i], &port) || port < 0 || port > 65535) return Usage();
      port_given = true;
    } else if (std::strcmp(argv[i], "--dev-root") == 0 && i + 1 < argc) {
      dev_root = argv[++i];
    } else if (argv[i][0] != '-' && clip_path.empty()) {
      clip_path = argv[i];
    } else {
      return Usage();
    }
  }
  if ((no_meta && !meta_path.empty()) || (clip_path.empty() && (no_meta || !meta_path.empty()))) return Usage();

  HttpServer server;
  PreviewService service(&server, dev_root);

  std::string error;
  if (!clip_path.empty()) {
    std::vector<uint8_t> data;
    if (!ReadFile(clip_path, &data, &error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    // The sidecar: as `inspect` and `verify` find it.
    std::string meta_text;
    std::string meta_desc = "none (auto mode)";
    bool has_meta = false;
    std::string sidecar = meta_path;
    if (!no_meta && sidecar.empty() && FileExists(SidecarPathFor(clip_path))) sidecar = SidecarPathFor(clip_path);
    if (no_meta) {
      meta_desc = "none (--no-meta: auto mode)";
    } else if (!sidecar.empty()) {
      if (!ReadTextFile(sidecar, &meta_text, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
      }
      has_meta = !meta_text.empty();
      meta_desc = sidecar + (meta_path.empty() ? " (next to the clip)" : " (--meta)");
    }
    Session* s = service.Add(FileNameOf(clip_path), std::move(data));
    ac_status status = AC_OK;
    if (!service.Open(s, meta_text, has_meta, meta_desc, &error, &status)) {
      // Not fatal: the page shows the reason and lets the sidecar be corrected.
      std::fprintf(stderr, "warning: the core refuses the clip: %s\n", error.c_str());
    }
  }

  if (!server.Listen(static_cast<int>(port), &error)) {
    if (port_given || !server.Listen(0, &error)) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
  }
  const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  std::printf("url: %s\n", url.c_str());
  if (!clip_path.empty()) std::printf("clip: %s\n", clip_path.c_str());
  std::printf("Press Ctrl+C to stop.\n");
  std::fflush(stdout);
  if (!no_open && !OpenInBrowser(url)) {
    std::fprintf(stderr, "warning: could not start a browser; open %s yourself\n", url.c_str());
  }
  server.Serve([&service](HttpRequest& req, HttpResponse* res) { service.Handle(req, res); });
  return 0;
}

}  // namespace cli
