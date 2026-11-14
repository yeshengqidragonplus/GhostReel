// The authoring commands of the command-line tool: inspect, verify, encode (M5) and preview (M6).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

// Prints the usage text to stderr and returns 2.
int Usage();

// argv[0] = program, argv[1] = the command, argv[2..] = its arguments.
int CmdInspect(int argc, char** argv);
int CmdVerify(int argc, char** argv);
int CmdEncode(int argc, char** argv);
int CmdPreview(int argc, char** argv);

// The `inspect` report ("key: value" lines) of a clip held in memory. `file_label` is printed as the file,
// `meta_text` is the sidecar ("" for none) and `meta_desc` says where it came from. False when the bytes cannot be
// read as WebM; `out` then holds the reason.
bool InspectReport(const std::vector<uint8_t>& data, const std::string& file_label, const std::string& meta_text,
                   const std::string& meta_desc, std::string* out);

struct VerifyOptions {
  std::string clip_path;
  // Sidecar: by default `<clip>.acmeta.json` next to the clip when it exists. Exactly one of these overrides that.
  std::string meta_path;            // --meta
  bool no_meta = false;             // --no-meta: open without a sidecar ("auto" mode)
  bool has_meta_text = false;       // the caller already has the text (encode)
  std::string meta_text;
  // The reference decoder. Without --ffmpeg, ALPHACLIP_FFMPEG or an ffmpeg on PATH is used when there is one; when
  // none can be started the comparison is skipped (it fails only when --ffmpeg was given explicitly).
  std::string ffmpeg;
  bool ffmpeg_explicit = false;
  bool no_ffmpeg = false;
};

// Runs every check and prints the report to stdout. Returns 0 when nothing failed (warnings do not fail), 1 otherwise.
int RunVerify(const VerifyOptions& options);

}  // namespace cli
