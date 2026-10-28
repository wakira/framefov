#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_set>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace framefov {
namespace log {

namespace {

std::atomic<Level> g_level{LEVEL_INFO};
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_triedInit = false;
bool g_stderrAll = false;
std::string g_path;
std::unordered_set<std::string> g_once;

const char* levelName(Level l) {
  switch (l) {
    case LEVEL_ERROR: return "ERROR";
    case LEVEL_WARN: return "WARN ";
    case LEVEL_INFO: return "INFO ";
    case LEVEL_DEBUG: return "DEBUG";
    case LEVEL_TRACE: return "TRACE";
  }
  return "?????";
}

std::string homeDir() {
  const char* h = getenv("HOME");
  return (h && *h) ? std::string(h) : std::string("/tmp");
}

std::string baseNameOf(const std::string& p) {
  const size_t slash = p.find_last_of('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Identifies the process in the log: the layer is loaded once per process and
// several processes (game, compositor, runtime) share the log file.
std::string processIdentity() {
  std::string exe = "?";
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    exe = baseNameOf(buf);
  }
  std::string cmdline = exe;
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if (f) {
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    const std::string first(buf, strnlen(buf, got));
    if (!first.empty()) {
      const std::string base = baseNameOf(first);
      if (base != exe) cmdline += " [" + base + "]";
    }
  }
  return cmdline;
}

bool mkdirRecursive(const std::string& dir) {
  std::string cur;
  cur.reserve(dir.size());
  size_t i = 0;
  if (!dir.empty() && dir[0] == '/') {
    cur.push_back('/');
    i = 1;
  }
  while (i < dir.size()) {
    size_t next = dir.find('/', i);
    if (next == std::string::npos) next = dir.size();
    if (next > i) {
      cur.append(dir, i, next - i);
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    if (next < dir.size()) cur.push_back('/');
    i = next + 1;
  }
  return true;
}

void formatTime(char* out, size_t n) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  snprintf(out, n, "%lld.%03ld", (long long)ts.tv_sec, ts.tv_nsec / 1000000L);
}

}  // namespace

void init() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_triedInit) return;
  g_triedInit = true;

  const char* xdg = getenv("XDG_STATE_HOME");
  std::string dir = (xdg && *xdg) ? std::string(xdg) : (homeDir() + "/.local/state");
  dir += "/framefov";
  if (!mkdirRecursive(dir)) {
    fprintf(stderr, "[framefov] cannot create log directory %s\n", dir.c_str());
    return;
  }
  g_path = dir + "/framefov.log";

  // Append, never truncate: every process that loads the layer writes its own
  // session header, and several processes share the file. Rotate at 16 MB, keeping
  // three generations, so that unrelated processes cannot push a session's log out
  // of the file before it can be read (a test run used to be able to overflow the
  // single .1 generation and delete the game's log).
  struct stat st;
  if (::stat(g_path.c_str(), &st) == 0 && st.st_size > 16 * 1024 * 1024) {
    for (int i = 3; i >= 1; --i) {
      const std::string from = i == 1 ? g_path : g_path + "." + std::to_string(i - 1);
      const std::string to = g_path + "." + std::to_string(i);
      remove(to.c_str());
      rename(from.c_str(), to.c_str());
    }
  }

  g_file = fopen(g_path.c_str(), "a");
  if (!g_file) {
    fprintf(stderr, "[framefov] cannot open log file %s\n", g_path.c_str());
    g_path.clear();
    return;
  }
  const char* stderrAll = getenv("FRAMEFOV_LOG_STDERR");
  g_stderrAll = stderrAll && *stderrAll && strcmp(stderrAll, "0") != 0;
  fprintf(g_file, "==== framefov [%s] pid %d %s ====\n", FRAMEFOV_LOG_TAG, (int)getpid(),
          processIdentity().c_str());
  fflush(g_file);
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
}

void flush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) fflush(g_file);
  fflush(stderr);
}

void setLevel(Level l) { g_level.store(l, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

void setLevelFromString(const char* s) {
  if (!s) return;
  std::string v(s);
  for (auto& c : v) c = (char)tolower((unsigned char)c);
  if (v == "error") setLevel(LEVEL_ERROR);
  else if (v == "warn" || v == "warning") setLevel(LEVEL_WARN);
  else if (v == "info") setLevel(LEVEL_INFO);
  else if (v == "debug") setLevel(LEVEL_DEBUG);
  else if (v == "trace") setLevel(LEVEL_TRACE);
}

void write(Level l, const char* tag, const char* file, int line, const char* fmt, ...) {
  if (l > g_level.load(std::memory_order_relaxed)) return;

  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char tbuf[64];
  formatTime(tbuf, sizeof(tbuf));

  const char* base = file ? strrchr(file, '/') : nullptr;
  base = base ? base + 1 : (file ? file : "?");

  // Strip the directory prefix and the .cpp extension for brevity: "vk_layer.cpp:123".
  char loc[128];
  size_t len = strlen(base);
  if (len > 4 && strcmp(base + len - 4, ".cpp") == 0) len -= 4;
  snprintf(loc, sizeof(loc), "%.*s:%d", (int)len, base, line);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) {
    fprintf(g_file, "%s %s [%s] %s %s\n", tbuf, levelName(l), tag, loc, msg);
    fflush(g_file);
  }
  if (l <= LEVEL_WARN || g_stderrAll) {
    fprintf(stderr, "[framefov] %s %s [%s] %s %s\n", tbuf, levelName(l), tag, loc, msg);
    fflush(stderr);
  }
}

bool once(const char* key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_once.insert(std::string(key)).second;
}

const char* path() { return g_path.c_str(); }

}  // namespace log
}  // namespace framefov
