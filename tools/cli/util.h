// Small helpers shared by the command-line tool's commands: UTF-8 arguments and paths on every platform, file
// access, and starting an external program (ffmpeg / ffprobe, which `encode` and `verify` use but never link).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace cli {

// argv as UTF-8. On Windows main() receives the ANSI code page, which cannot hold every path, so the wide command
// line is converted instead.
std::vector<std::string> Utf8Args(int argc, char** argv);

bool ParseInt64(const char* text, int64_t* out);
bool ParseDouble(const char* text, double* out);

// UTF-8 paths on every platform.
std::FILE* OpenFile(const std::string& path, const char* mode);
bool ReadFile(const std::string& path, std::vector<uint8_t>* out, std::string* error);  // at most 1 GiB
bool ReadTextFile(const std::string& path, std::string* out, std::string* error);       // at most 1 MiB
bool WriteTextFile(const std::string& path, const std::string& text, std::string* error);
bool FileExists(const std::string& path);
bool IsDirectory(const std::string& path);
bool RemoveFile(const std::string& path);
// Creates the folder that will hold `path` (and its parents) when it does not exist yet.
bool EnsureParentDirectory(const std::string& path, std::string* error);
bool MoveReplace(const std::string& from, const std::string& to, std::string* error);  // rename, replacing `to`
int64_t FileSize(const std::string& path);                                              // -1 when unknown

// clip.webm -> clip.acmeta.json (same folder, same name)
std::string SidecarPathFor(const std::string& clip_path);
// "dir/clip" for "dir/clip.webm": everything but the last extension of the file name
std::string StripExtension(const std::string& path);
std::string FileNameOf(const std::string& path);

// The ffmpeg to run: an explicit option (a file, or a folder holding ffmpeg), else the ALPHACLIP_FFMPEG environment
// variable, else "ffmpeg" from PATH. ffprobe is looked up next to it.
std::string FfmpegPath(const std::string& option);
std::string FfprobePath(const std::string& ffmpeg);

// A child process. stderr (and stdout, unless captured) are the tool's own; stdin is the null device.
class Child {
 public:
  Child();
  ~Child();  // ends the child if it is still running
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  bool Start(const std::vector<std::string>& argv, bool capture_stdout, std::string* error);
  // Reads up to `n` bytes of the captured stdout, blocking until `n` are there or the stream ends.
  size_t ReadFully(void* buf, size_t n);
  // Waits for the child to end and returns its exit code (-1 when that cannot be determined).
  int Wait();
  void Terminate();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Runs a program to completion and returns everything it wrote to stdout. False when it cannot be started or exits
// with a non-zero code (`error` then says why).
bool RunCapture(const std::vector<std::string>& argv, std::string* out, std::string* error);

}  // namespace cli
