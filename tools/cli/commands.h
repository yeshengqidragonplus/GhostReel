// The authoring commands of the command-line tool (M5): inspect, verify, encode.
#pragma once

#include <string>

namespace cli {

// Prints the usage text to stderr and returns 2.
int Usage();

// argv[0] = program, argv[1] = the command, argv[2..] = its arguments.
int CmdInspect(int argc, char** argv);
int CmdVerify(int argc, char** argv);
int CmdEncode(int argc, char** argv);

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
