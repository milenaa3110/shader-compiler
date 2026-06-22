// error_utils_fmt.h — format-string error logger, depends on libfmt.
// Use this header from host-compiled translation units that link fmt::fmt.
// Cross-compiled (RISC-V) sources should use plain "error_utils.h".
#ifndef COMPILER_GLSL_ERROR_UTILS_FMT_H
#define COMPILER_GLSL_ERROR_UTILS_FMT_H

#include "error_utils.h"

#include <fmt/format.h>
#include <string_view>

template <typename... Args>
inline void logErrorFmt(const std::string& fmtStr, Args&&... args) {
    logError(fmt::format(fmt::runtime(fmtStr), std::forward<Args>(args)...));
}

inline void logErrorContext(const std::string& context, const std::string& msg) {
    logError(fmt::format("{}: {}", context, msg));
}

// ─── Source-buffer registry for caret diagnostics ──────────────────────────
//
// Held as file-scope state because every TU includes this header and the
// compilers are single-threaded. Set once by the entry point right before
// ParseProgram, used by logErrorAt to underline the offending column.
// Empty buffer ⇒ caret rendering is skipped and the old plain-text format
// is used — keeps fallback behaviour for tools that don't track source.
namespace diag {

struct SourceContext {
    std::string_view buffer;
    std::string      name = "<stdin>";
};

inline SourceContext& currentSource() {
    static SourceContext ctx;
    return ctx;
}

inline void setSource(std::string_view buf, std::string_view name = "<stdin>") {
    auto& ctx = currentSource();
    ctx.buffer = buf;
    ctx.name   = std::string(name);
}

inline void clearSource() { currentSource() = {}; }

// Locate line `line` (1-based) in the buffer; returns the substring or an
// empty view if the line doesn't exist.
inline std::string_view extractLine(std::string_view buf, int line) {
    if (line <= 0 || buf.empty()) return {};
    size_t pos = 0;
    int    cur = 1;
    while (cur < line && pos < buf.size()) {
        if (buf[pos] == '\n') ++cur;
        ++pos;
    }
    if (cur != line) return {};
    size_t start = pos;
    while (pos < buf.size() && buf[pos] != '\n' && buf[pos] != '\r') ++pos;
    return buf.substr(start, pos - start);
}

}  // namespace diag

// Source-located error. With a registered source buffer the output looks
// like clang:
//
//     [ERROR] shader.src:6:13: Expected ';' after assignment
//         float x = 1.0
//                     ^
//
// Without one, falls back to the original one-liner so cross-compiled
// runtime code keeps working.
inline void logErrorAt(int line, int col, const std::string& msg) {
    auto& ctx = diag::currentSource();
    if (ctx.buffer.empty() || line <= 0) {
        logError(fmt::format("line {}, col {}: {}", line, col, msg));
        return;
    }
    auto src = diag::extractLine(ctx.buffer, line);
    std::cerr << fmt::format("[ERROR] {}:{}:{}: {}\n",
                             ctx.name, line, col, msg);
    if (!src.empty()) {
        std::cerr << "    " << src << '\n';
        // Pad with spaces up to `col` (1-based); fall back to 0 padding
        // for stamps that point past the line end.
        int pad = col > 0 ? col - 1 : 0;
        std::cerr << "    " << std::string(pad, ' ') << "^\n";
    }
}

// Source-located format-string error — logErrorFmt + position.
template <typename... Args>
inline void logErrorFmtAt(int line, int col, const std::string& fmtStr, Args&&... args) {
    logErrorAt(line, col, fmt::format(fmt::runtime(fmtStr), std::forward<Args>(args)...));
}

#endif  // COMPILER_GLSL_ERROR_UTILS_FMT_H
