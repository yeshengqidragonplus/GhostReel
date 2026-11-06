// `alphaclip encode <input> --out <clip.webm> [options]` (design doc TR-1).
//
// Turns footage into a playable package: `clip.webm` (standard VP9 WebM) plus `clip.acmeta.json` (the sidecar the
// bindings read), then runs `verify` on it. The encoding itself is done by an EXTERNAL ffmpeg (libvpx-vp9) that this tool
// starts as a program: ffmpeg is GPL-licensed, so it is never linked or shipped, only used when it is there.
//
//   <input>   a video file (with or without alpha: MOV/ProRes 4444, WebM with VP9 alpha, ...) or an image sequence
//             as a printf pattern (frames/shot_%04d.png, needs --fps). The alpha of RGBA PNGs is used.
//   --mode    auto (default: native_alpha when the input has alpha, otherwise opaque), native_alpha, packed_right,
//             packed_bottom, opaque
//   --straight  keep the colour as it is (the sidecar says premultiplied=false). Default: the colour is multiplied
//             by alpha before encoding (premultiplied=true), which 4:2:0 chroma subsampling needs to stay clean at soft
//             edges. No edge padding is done.
//   --fps N   frame rate (a number or a fraction such as 30000/1001); required for image sequences, otherwise it
//             resamples the video
//   --crf N   quality, 0 (best) .. 63; default 24
//   --gop N   frames between keyframes (a fixed GOP: the colour and alpha streams then have their keyframes in the
//             same places); default: two seconds
//   --matrix bt601|bt709 (default bt601), --range limited|full (default limited): written into the stream and sidecar
//   --loop    loop_hint=true in the sidecar
//   --cpu-used N (default 2, 0 = slowest/best .. 8), --threads N
//   --ffmpeg <file or folder>  (else ALPHACLIP_FFMPEG, else ffmpeg on PATH; ffprobe is taken from next to it)
//   --force   overwrite existing output;  --no-verify  skip the check at the end;  --quiet / --verbose
// Exit code 0 on success, 1 on failure, 2 on a usage error.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "alphaclip.h"
#include "commands.h"
#include "util.h"

namespace cli {
namespace {

struct Options {
  std::string input;
  std::string out;
  std::string mode = "auto";
  bool straight = false;
  std::string fps_text;  // as given: handed to ffmpeg unchanged
  double fps = 0;
  int crf = 24;
  int gop = 0;
  std::string matrix = "bt601";
  std::string range = "limited";
  bool loop = false;
  int cpu_used = 2;
  int threads = 0;
  std::string ffmpeg;
  bool ffmpeg_explicit = false;
  bool force = false;
  bool no_verify = false;
  bool quiet = false;
  bool verbose = false;
};

struct Probe {
  std::string codec;
  std::string pix_fmt;
  int width = 0;
  int height = 0;
  double fps = 0;
  bool alpha_tag = false;  // WebM: the stream says AlphaMode=1
};

int Fail(const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  return 1;
}

bool ParseRate(const std::string& text, double* out) {
  const size_t slash = text.find('/');
  if (slash == std::string::npos) return ParseDouble(text.c_str(), out) && *out > 0;
  double num = 0, den = 0;
  if (!ParseDouble(text.substr(0, slash).c_str(), &num) || !ParseDouble(text.substr(slash + 1).c_str(), &den)) {
    return false;
  }
  if (den <= 0 || num <= 0) return false;
  *out = num / den;
  return true;
}

bool PixFmtHasAlpha(const std::string& f) {
  for (const char* prefix : {"yuva", "gbrap", "rgba", "bgra", "argb", "abgr", "ya8", "ya16", "ayuv", "vuya", "rgbaf", "bgraf"}) {
    if (f.compare(0, std::strlen(prefix), prefix) == 0) return true;
  }
  return false;
}

std::string Join(const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& a : v) {
    if (!s.empty()) s += ' ';
    s += a.find(' ') != std::string::npos ? "\"" + a + "\"" : a;
  }
  return s;
}

bool IsSequence(const std::string& input) { return input.find('%') != std::string::npos; }

// Input options that must come before `-i` (the same for ffprobe and ffmpeg).
std::vector<std::string> InputOptions(const Options& o, const Probe* probe) {
  std::vector<std::string> v;
  if (IsSequence(o.input)) {
    v.push_back("-framerate");
    v.push_back(o.fps_text);
  }
  // ffmpeg's native VP9 decoder drops the alpha of a WebM; libvpx's decodes it.
  if (probe != nullptr && probe->alpha_tag && probe->codec == "vp9") {
    v.push_back("-c:v");
    v.push_back("libvpx-vp9");
  }
  return v;
}

bool ProbeInput(const Options& o, const std::string& ffprobe, Probe* p, std::string* error) {
  std::vector<std::string> args = {ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                                   "stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate:stream_tags=alpha_mode",
                                   "-of", "default=noprint_wrappers=1"};
  const std::vector<std::string> in = InputOptions(o, nullptr);
  args.insert(args.end(), in.begin(), in.end());
  args.push_back("-i");
  args.push_back(o.input);
  std::string out;
  if (!RunCapture(args, &out, error)) return false;
  std::map<std::string, std::string> kv;
  size_t pos = 0;
  while (pos < out.size()) {
    size_t eol = out.find_first_of("\r\n", pos);
    if (eol == std::string::npos) eol = out.size();
    const std::string line = out.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq);
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));  // ffprobe prints TAG:ALPHA_MODE
    kv[key] = line.substr(eq + 1);
  }
  if (kv.find("width") == kv.end() || kv.find("height") == kv.end()) {
    *error = "ffprobe found no video stream in '" + o.input + "'";
    return false;
  }
  p->codec = kv["codec_name"];
  p->pix_fmt = kv["pix_fmt"];
  p->width = std::atoi(kv["width"].c_str());
  p->height = std::atoi(kv["height"].c_str());
  double rate = 0;
  if (!ParseRate(kv["avg_frame_rate"], &rate)) ParseRate(kv["r_frame_rate"], &rate);
  p->fps = rate;
  p->alpha_tag = kv["tag:alpha_mode"] == "1";
  return p->width > 0 && p->height > 0;
}

std::string ColorScale(const Options& o) {
  return "scale=out_color_matrix=" + o.matrix + ":out_range=" + (o.range == "full" ? "pc" : "tv");
}

std::string BuildFilter(const Options& o, const std::string& mode, bool override_fps) {
  const std::string head = std::string("[0:v]") + (override_fps ? "fps=" + o.fps_text + "," : "") +
                           // everything goes through RGB so that the matrix conversion is explicit and correct
                           // for YUV sources too (swscale does not convert between YUV matrices)
                           "format=gbrap";
  const std::string premultiply = o.straight ? "" : "premultiply=inplace=1,";
  if (mode == "native_alpha") {
    return head + (o.straight ? "" : ",premultiply=inplace=1") + "," + ColorScale(o) + ",format=yuva420p[v]";
  }
  if (mode == "opaque") return head + "," + ColorScale(o) + ",format=yuv420p[v]";
  // packed: the colour (alpha is dropped by the conversion) next to the alpha as a grey image, stored as luma in the
  // clip's range
  const std::string stack = mode == "packed_right" ? "hstack" : "vstack";
  return head + ",split=2[x][y];[x]" + premultiply + ColorScale(o) + ",format=yuv420p[c];[y]alphaextract,scale=in_range=pc:out_range=" +
         (o.range == "full" ? "pc" : "tv") + ",format=yuv420p[a];[c][a]" + stack + "[v]";
}

std::string MetaJson(const Options& o, const std::string& mode, int w, int h) {
  std::string s = "{\n";
  s += "  \"schema\": 1,\n";
  s += "  \"alpha_mode\": \"" + mode + "\",\n";
  s += std::string("  \"premultiplied\": ") + (mode != "opaque" && !o.straight ? "true" : "false") + ",\n";
  s += "  \"color_range\": \"" + o.range + "\",\n";
  s += "  \"matrix\": \"" + o.matrix + "\",\n";
  s += "  \"content_size\": [" + std::to_string(w) + ", " + std::to_string(h) + "],\n";
  s += std::string("  \"loop_hint\": ") + (o.loop ? "true" : "false") + ",\n";
  s += std::string("  \"generator\": \"alphaclip encode, core ") + ac_version() + "\"\n";
  s += "}\n";
  return s;
}

bool NeedValue(int i, int argc) { return i + 1 < argc; }

}  // namespace

int CmdEncode(int argc, char** argv) {
  Options o;
  o.input = argv[2];
  for (int i = 3; i < argc; ++i) {
    const char* a = argv[i];
    int64_t v = 0;
    if (std::strcmp(a, "--out") == 0 && NeedValue(i, argc)) o.out = argv[++i];
    else if (std::strcmp(a, "--mode") == 0 && NeedValue(i, argc)) o.mode = argv[++i];
    else if (std::strcmp(a, "--straight") == 0) o.straight = true;
    else if (std::strcmp(a, "--fps") == 0 && NeedValue(i, argc)) {
      o.fps_text = argv[++i];
      if (!ParseRate(o.fps_text, &o.fps)) return Usage();
    } else if (std::strcmp(a, "--crf") == 0 && NeedValue(i, argc)) {
      if (!ParseInt64(argv[++i], &v) || v < 0 || v > 63) return Usage();
      o.crf = static_cast<int>(v);
    } else if (std::strcmp(a, "--gop") == 0 && NeedValue(i, argc)) {
      if (!ParseInt64(argv[++i], &v) || v < 1 || v > 100000) return Usage();
      o.gop = static_cast<int>(v);
    } else if (std::strcmp(a, "--matrix") == 0 && NeedValue(i, argc)) o.matrix = argv[++i];
    else if (std::strcmp(a, "--range") == 0 && NeedValue(i, argc)) o.range = argv[++i];
    else if (std::strcmp(a, "--loop") == 0) o.loop = true;
    else if (std::strcmp(a, "--cpu-used") == 0 && NeedValue(i, argc)) {
      if (!ParseInt64(argv[++i], &v) || v < 0 || v > 8) return Usage();
      o.cpu_used = static_cast<int>(v);
    } else if (std::strcmp(a, "--threads") == 0 && NeedValue(i, argc)) {
      if (!ParseInt64(argv[++i], &v) || v < 1 || v > 64) return Usage();
      o.threads = static_cast<int>(v);
    } else if (std::strcmp(a, "--ffmpeg") == 0 && NeedValue(i, argc)) {
      o.ffmpeg = argv[++i];
      o.ffmpeg_explicit = true;
    } else if (std::strcmp(a, "--force") == 0) o.force = true;
    else if (std::strcmp(a, "--no-verify") == 0) o.no_verify = true;
    else if (std::strcmp(a, "--quiet") == 0) o.quiet = true;
    else if (std::strcmp(a, "--verbose") == 0) o.verbose = true;
    else return Usage();
  }
  if (o.out.empty()) return Usage();
  if (o.mode != "auto" && o.mode != "native_alpha" && o.mode != "packed_right" && o.mode != "packed_bottom" &&
      o.mode != "opaque") {
    return Fail("--mode must be auto, native_alpha, packed_right, packed_bottom or opaque");
  }
  if (o.matrix != "bt601" && o.matrix != "bt709") return Fail("--matrix must be bt601 or bt709");
  if (o.range != "limited" && o.range != "full") return Fail("--range must be limited or full");
  if (IsSequence(o.input)) {
    if (o.fps_text.empty()) return Fail("an image sequence needs --fps (for example --fps 30)");
  } else if (!FileExists(o.input)) {
    return Fail("the input '" + o.input + "' does not exist");
  }
  const std::string meta_path = SidecarPathFor(o.out);
  if (!o.force) {
    if (FileExists(o.out)) return Fail("'" + o.out + "' already exists (use --force to replace it)");
    if (FileExists(meta_path)) return Fail("'" + meta_path + "' already exists (use --force to replace it)");
  }

  const std::string ffmpeg = FfmpegPath(o.ffmpeg);
  const std::string ffprobe = FfprobePath(ffmpeg);
  std::string error;
  Probe probe;
  if (!ProbeInput(o, ffprobe, &probe, &error)) {
    return Fail(error + " (ffmpeg and ffprobe are needed to encode: give --ffmpeg <path> or set ALPHACLIP_FFMPEG)");
  }
  const bool input_alpha = PixFmtHasAlpha(probe.pix_fmt) || (probe.alpha_tag && probe.codec == "vp9");

  std::string mode = o.mode;
  if (mode == "auto") mode = input_alpha ? "native_alpha" : "opaque";
  if (mode != "opaque" && !input_alpha) {
    return Fail("the input has no alpha channel (pixel format '" + probe.pix_fmt + "'): there is nothing to store as " +
                mode + "; use --mode opaque");
  }
  const bool packed = mode == "packed_right" || mode == "packed_bottom";
  if (packed && (probe.width % 2 != 0 || probe.height % 2 != 0)) {
    return Fail("the picture size " + std::to_string(probe.width) + "x" + std::to_string(probe.height) +
                " is odd; the packed modes need an even size (crop or scale the footage first)");
  }

  double fps = o.fps > 0 ? o.fps : probe.fps;
  std::string fps_text = o.fps_text;
  if (fps <= 0) return Fail("the frame rate of the input is unknown; give --fps");
  const int gop = o.gop > 0 ? o.gop : std::max(1, static_cast<int>(std::lround(2.0 * fps)));
  const bool override_fps = !IsSequence(o.input) && o.fps > 0;
  Options eff = o;
  if (fps_text.empty()) eff.fps_text = std::to_string(fps);

  const bool alpha_stream = mode == "native_alpha";
  const int threads = o.threads > 0 ? o.threads : static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
  const std::string part = o.out + ".part";

  std::vector<std::string> cmd = {ffmpeg, "-nostdin", "-hide_banner", "-v", "error"};
  if (!o.quiet) cmd.push_back("-stats");
  cmd.push_back("-y");
  for (const std::string& s : InputOptions(o, &probe)) cmd.push_back(s);
  cmd.insert(cmd.end(), {"-i", o.input, "-filter_complex", BuildFilter(eff, mode, override_fps), "-map", "[v]", "-an", "-sn", "-dn"});
  cmd.insert(cmd.end(), {"-fps_mode", "cfr", "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", std::to_string(o.crf), "-g",
                         std::to_string(gop), "-keyint_min", std::to_string(gop), "-auto-alt-ref", "0",
                         "-lag-in-frames", "0", "-deadline", "good", "-cpu-used", std::to_string(o.cpu_used), "-row-mt",
                         "1", "-threads", std::to_string(threads), "-pix_fmt", alpha_stream ? "yuva420p" : "yuv420p"});
  if (o.matrix == "bt709") {
    cmd.insert(cmd.end(), {"-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"});
  } else {
    cmd.insert(cmd.end(), {"-colorspace", "smpte170m", "-color_primaries", "smpte170m", "-color_trc", "smpte170m"});
  }
  cmd.insert(cmd.end(), {"-color_range", o.range == "full" ? "pc" : "tv"});
  if (alpha_stream) cmd.insert(cmd.end(), {"-metadata:s:v:0", "alpha_mode=1"});
  cmd.insert(cmd.end(), {"-fflags", "+bitexact", "-f", "webm", part});

  std::printf("encoding %s -> %s (%s, %dx%d, %.3f fps, crf %d, keyframe every %d frames, %s colour)\n", o.input.c_str(),
              o.out.c_str(), mode.c_str(), probe.width, probe.height, fps, o.crf, gop,
              mode == "opaque" ? "opaque" : (o.straight ? "straight" : "premultiplied"));
  if (o.verbose) std::printf("%s\n", Join(cmd).c_str());
  std::fflush(stdout);

  if (!EnsureParentDirectory(o.out, &error)) return Fail(error);
  RemoveFile(part);
  Child child;
  if (!child.Start(cmd, /*capture_stdout=*/false, &error)) return Fail(error);
  const int code = child.Wait();
  if (!o.quiet) std::fputc('\n', stderr);
  if (code != 0) {
    RemoveFile(part);
    return Fail("ffmpeg failed (exit code " + std::to_string(code) + "); its message is above");
  }

  const std::string meta = MetaJson(eff, mode, probe.width, probe.height);
  if (!o.no_verify) {
    VerifyOptions vo;
    vo.clip_path = part;
    vo.has_meta_text = true;
    vo.meta_text = meta;
    vo.ffmpeg = o.ffmpeg;
    vo.ffmpeg_explicit = o.ffmpeg_explicit;
    std::printf("\n");
    if (RunVerify(vo) != 0) {
      std::fprintf(stderr, "error: the encoded clip failed verification; it was kept as '%s' and no sidecar was written\n",
                   part.c_str());
      return 1;
    }
    std::printf("\n");
  }

  if (!MoveReplace(part, o.out, &error)) return Fail(error);
  if (!WriteTextFile(meta_path, meta, &error)) return Fail(error);
  std::printf("wrote %s (%lld bytes) and %s\n", o.out.c_str(), static_cast<long long>(FileSize(o.out)), meta_path.c_str());
  return 0;
}

}  // namespace cli
