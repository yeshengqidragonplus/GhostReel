#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cli {
namespace {

fs::path P(const std::string& utf8) { return fs::u8path(utf8); }

#ifdef _WIN32
std::string ToUtf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring ToWide(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 1 ? static_cast<size_t>(n - 1) : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// The quoting rules CommandLineToArgvW / the C runtime undo.
std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

std::string LastErrorText() {
  const DWORD code = GetLastError();
  char* msg = nullptr;
  const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
  std::string text = "error " + std::to_string(code);
  if (n > 0 && msg != nullptr) {
    std::string m(msg, n);
    while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ' || m.back() == '.')) m.pop_back();
    text = m + " (" + text + ")";
  }
  if (msg != nullptr) LocalFree(msg);
  return text;
}
#endif

}  // namespace

std::vector<std::string> Utf8Args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv != nullptr) {
    for (int i = 0; i < wargc; ++i) out.push_back(ToUtf8(wargv[i]));
    LocalFree(wargv);
    return out;
  }
#endif
  for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
  return out;
}

bool ParseInt64(const char* text, int64_t* out) {
  if (text == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseDouble(const char* text, double* out) {
  if (text == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !std::isfinite(v)) return false;
  *out = v;
  return true;
}

std::FILE* OpenFile(const std::string& path, const char* mode) {
#ifdef _WIN32
  std::wstring wmode;
  for (const char* c = mode; *c != '\0'; ++c) wmode.push_back(static_cast<wchar_t>(*c));
  return _wfopen(P(path).c_str(), wmode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

bool ReadFile(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
  std::error_code ec;
  const uintmax_t size = fs::file_size(P(path), ec);
  if (ec) {
    *error = "cannot open '" + path + "': " + ec.message();
    return false;
  }
  if (size > (1ull << 30)) {
    *error = "'" + path + "' is larger than 1 GiB";
    return false;
  }
  std::FILE* f = OpenFile(path, "rb");
  if (f == nullptr) {
    *error = "cannot open '" + path + "'";
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t n = size == 0 ? 0 : std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (n != out->size()) {
    *error = "cannot read '" + path + "'";
    return false;
  }
  return true;
}

bool ReadTextFile(const std::string& path, std::string* out, std::string* error) {
  std::vector<uint8_t> bytes;
  std::error_code ec;
  const uintmax_t size = fs::file_size(P(path), ec);
  if (!ec && size > (1u << 20)) {
    *error = "'" + path + "' is larger than 1 MiB";
    return false;
  }
  if (!ReadFile(path, &bytes, error)) return false;
  out->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string* error) {
  std::FILE* f = OpenFile(path, "wb");
  if (f == nullptr) {
    *error = "cannot create '" + path + "'";
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool closed = std::fclose(f) == 0;
  if (!ok || !closed) {
    *error = "cannot write to '" + path + "'";
    return false;
  }
  return true;
}

bool FileExists(const std::string& path) {
  std::error_code ec;
  return fs::exists(P(path), ec) && !ec;
}

bool IsDirectory(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(P(path), ec) && !ec;
}

bool RemoveFile(const std::string& path) {
  std::error_code ec;
  fs::remove(P(path), ec);
  return !ec;
}

bool EnsureParentDirectory(const std::string& path, std::string* error) {
  std::error_code ec;
  const fs::path parent = P(path).parent_path();
  if (parent.empty() || fs::exists(parent, ec)) return true;
  fs::create_directories(parent, ec);
  if (ec) {
    *error = "cannot create the folder '" + parent.u8string() + "': " + ec.message();
    return false;
  }
  return true;
}

bool MoveReplace(const std::string& from, const std::string& to, std::string* error) {
#ifdef _WIN32
  if (!MoveFileExW(P(from).c_str(), P(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    *error = "cannot move '" + from + "' to '" + to + "': " + LastErrorText();
    return false;
  }
  return true;
#else
  std::error_code ec;
  fs::rename(P(from), P(to), ec);
  if (ec) {
    *error = "cannot move '" + from + "' to '" + to + "': " + ec.message();
    return false;
  }
  return true;
#endif
}

int64_t FileSize(const std::string& path) {
  std::error_code ec;
  const uintmax_t size = fs::file_size(P(path), ec);
  return ec ? -1 : static_cast<int64_t>(size);
}

std::string StripExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == 0) return path;
  if (slash != std::string::npos && dot == slash + 1) return path;  // ".hidden"
  return path.substr(0, dot);
}

std::string FileNameOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string SidecarPathFor(const std::string& clip_path) { return StripExtension(clip_path) + ".acmeta.json"; }

std::string FfmpegPath(const std::string& option) {
  std::string base = option;
  if (base.empty()) {
    const char* env = std::getenv("ALPHACLIP_FFMPEG");
    if (env != nullptr) base = env;
  }
  if (base.empty()) return "ffmpeg";
  if (IsDirectory(base)) {
#ifdef _WIN32
    return (P(base) / "ffmpeg.exe").u8string();
#else
    return (P(base) / "ffmpeg").u8string();
#endif
  }
  return base;
}

std::string FfprobePath(const std::string& ffmpeg) {
  const fs::path p = P(ffmpeg);
  if (p.has_parent_path()) return (p.parent_path() / ("ffprobe" + p.extension().string())).u8string();
  return "ffprobe";
}

// ---- Child process -------------------------------------------------------------------------------------------------

struct Child::Impl {
  bool started = false;
  bool waited = false;
  int exit_code = -1;
#ifdef _WIN32
  HANDLE process = nullptr;
  HANDLE read_pipe = nullptr;
#else
  pid_t pid = -1;
  int read_fd = -1;
#endif
};

Child::Child() : impl_(new Impl()) {}

Child::~Child() {
  if (impl_->started && !impl_->waited) {
    Terminate();
    Wait();
  }
}

#ifdef _WIN32

bool Child::Start(const std::vector<std::string>& argv, bool capture_stdout, std::string* error) {
  if (argv.empty()) {
    *error = "no program to run";
    return false;
  }
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = TRUE;

  HANDLE out_r = nullptr;
  HANDLE out_w = nullptr;
  if (capture_stdout) {
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
      *error = "cannot create a pipe: " + LastErrorText();
      return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  }
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = capture_stdout ? out_w : GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  std::wstring cmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) cmd.push_back(L' ');
    cmd += QuoteArg(ToWide(argv[i]));
  }
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  std::fflush(nullptr);
  PROCESS_INFORMATION pi;
  std::memset(&pi, 0, sizeof(pi));
  const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  const std::string why = ok ? std::string() : LastErrorText();
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (out_w != nullptr) CloseHandle(out_w);
  if (!ok) {
    if (out_r != nullptr) CloseHandle(out_r);
    *error = "cannot start '" + argv[0] + "': " + why;
    return false;
  }
  CloseHandle(pi.hThread);
  impl_->process = pi.hProcess;
  impl_->read_pipe = out_r;
  impl_->started = true;
  return true;
}

size_t Child::ReadFully(void* buf, size_t n) {
  if (impl_->read_pipe == nullptr) return 0;
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    const DWORD want = static_cast<DWORD>(std::min<size_t>(n - got, 1u << 20));
    DWORD read = 0;
    if (!::ReadFile(impl_->read_pipe, p + got, want, &read, nullptr) || read == 0) break;  // ::, not cli::ReadFile
    got += read;
  }
  return got;
}

int Child::Wait() {
  if (!impl_->started) return -1;
  if (impl_->waited) return impl_->exit_code;
  if (impl_->read_pipe != nullptr) {
    CloseHandle(impl_->read_pipe);
    impl_->read_pipe = nullptr;
  }
  WaitForSingleObject(impl_->process, INFINITE);
  DWORD code = 0;
  impl_->exit_code = GetExitCodeProcess(impl_->process, &code) ? static_cast<int>(code) : -1;
  CloseHandle(impl_->process);
  impl_->process = nullptr;
  impl_->waited = true;
  return impl_->exit_code;
}

void Child::Terminate() {
  if (impl_->started && !impl_->waited && impl_->process != nullptr) TerminateProcess(impl_->process, 1);
}

#else  // POSIX

bool Child::Start(const std::vector<std::string>& argv, bool capture_stdout, std::string* error) {
  if (argv.empty()) {
    *error = "no program to run";
    return false;
  }
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);

  int out[2] = {-1, -1};
  int err[2] = {-1, -1};
  if (capture_stdout && pipe(out) != 0) {
    *error = std::string("cannot create a pipe: ") + std::strerror(errno);
    return false;
  }
  if (pipe(err) != 0) {
    *error = std::string("cannot create a pipe: ") + std::strerror(errno);
    if (out[0] >= 0) {
      close(out[0]);
      close(out[1]);
    }
    return false;
  }
  fcntl(err[1], F_SETFD, FD_CLOEXEC);  // closes on a successful exec: the parent then reads end of file
  std::fflush(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    *error = std::string("cannot start '") + argv[0] + "': " + std::strerror(errno);
    close(err[0]);
    close(err[1]);
    if (out[0] >= 0) {
      close(out[0]);
      close(out[1]);
    }
    return false;
  }
  if (pid == 0) {
    const int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) {
      dup2(nul, 0);
      close(nul);
    }
    if (capture_stdout) {
      dup2(out[1], 1);
      close(out[0]);
      close(out[1]);
    }
    close(err[0]);
    execvp(args[0], args.data());
    const int e = errno;
    const ssize_t w = write(err[1], &e, sizeof(e));
    (void)w;
    _exit(127);
  }
  close(err[1]);
  if (capture_stdout) close(out[1]);
  int e = 0;
  ssize_t r;
  do {
    r = read(err[0], &e, sizeof(e));
  } while (r < 0 && errno == EINTR);
  close(err[0]);
  if (r == static_cast<ssize_t>(sizeof(e))) {
    waitpid(pid, nullptr, 0);
    if (out[0] >= 0) close(out[0]);
    *error = std::string("cannot start '") + argv[0] + "': " + std::strerror(e);
    return false;
  }
  impl_->pid = pid;
  impl_->read_fd = capture_stdout ? out[0] : -1;
  impl_->started = true;
  return true;
}

size_t Child::ReadFully(void* buf, size_t n) {
  if (impl_->read_fd < 0) return 0;
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    const ssize_t r = read(impl_->read_fd, p + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += static_cast<size_t>(r);
  }
  return got;
}

int Child::Wait() {
  if (!impl_->started) return -1;
  if (impl_->waited) return impl_->exit_code;
  if (impl_->read_fd >= 0) {
    close(impl_->read_fd);
    impl_->read_fd = -1;
  }
  int status = 0;
  pid_t r;
  do {
    r = waitpid(impl_->pid, &status, 0);
  } while (r < 0 && errno == EINTR);
  if (r < 0) impl_->exit_code = -1;
  else if (WIFEXITED(status)) impl_->exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) impl_->exit_code = 128 + WTERMSIG(status);
  impl_->waited = true;
  return impl_->exit_code;
}

void Child::Terminate() {
  if (impl_->started && !impl_->waited && impl_->pid > 0) kill(impl_->pid, SIGKILL);
}

#endif

bool RunCapture(const std::vector<std::string>& argv, std::string* out, std::string* error) {
  Child child;
  if (!child.Start(argv, /*capture_stdout=*/true, error)) return false;
  out->clear();
  char buf[4096];
  for (;;) {
    const size_t n = child.ReadFully(buf, sizeof(buf));
    out->append(buf, n);
    if (n < sizeof(buf)) break;
  }
  const int code = child.Wait();
  if (code != 0) {
    *error = "'" + argv[0] + "' exited with code " + std::to_string(code);
    return false;
  }
  return true;
}

}  // namespace cli
