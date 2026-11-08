#ifndef ANGORA_LOG_H_
#define ANGORA_LOG_H_

#include <cstdarg>
#include <cstdio>

// Unified diagnostics for the angora tools.
//
// Every diagnostic goes to stderr in the shape
//   <tool>: [<file>:<line>: ]<severity>: <message>
// where <severity> is one of "error", "warning", "info". Messages carry no
// trailing punctuation. Program output (the VM `print` instruction) never goes
// through here: it stays on stdout. `location.file`/`location.line` are optional
// source context; line 0 or a null file means the field is omitted.
enum class LogSeverity { Error, Warning, Info };

struct LogLocation {
  const char *file = nullptr;  // source file name, or nullptr for none
  long line = 0;               // 1-based source line, or 0 for none
};

// Core writer: format `ap` and emit one diagnostic line. `tool` is the fixed
// program name ("angora-interp", "angora-vm", "angora-runtime").
inline void LogVWrite(LogSeverity sev, const char *tool, const LogLocation &loc, const char *fmt,
                      va_list ap) {
  const char *name = sev == LogSeverity::Error     ? "error"
                     : sev == LogSeverity::Warning ? "warning"
                                                   : "info";
  std::fprintf(stderr, "%s: ", tool);
  if (loc.file != nullptr) {
    std::fprintf(stderr, "%s:", loc.file);
    if (loc.line > 0) {
      std::fprintf(stderr, "%ld:", loc.line);
    }
    std::fputs(" ", stderr);
  }
  std::fprintf(stderr, "%s: ", name);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

// Variadic front-end for LogVWrite.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
inline void LogWrite(LogSeverity sev, const char *tool, const LogLocation &loc, const char *fmt,
                     ...) {
  va_list ap;
  va_start(ap, fmt);
  LogVWrite(sev, tool, loc, fmt, ap);
  va_end(ap);
}

#endif  // ANGORA_LOG_H_
