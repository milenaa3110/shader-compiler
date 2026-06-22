// error_utils_fmt.h — format-string error logger, depends on libfmt.
// Use this header from host-compiled translation units that link fmt::fmt.
// Cross-compiled (RISC-V) sources should use plain "error_utils.h".
#ifndef COMPILER_GLSL_ERROR_UTILS_FMT_H
#define COMPILER_GLSL_ERROR_UTILS_FMT_H

#include "error_utils.h"
#include "source_manager.h"

#include <fmt/format.h>
#include <string>
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

inline SourceManager& currentSource() {
    static SourceManager sm;
    return sm;
}

inline void setSource(std::string_view buf, std::string_view name = "<stdin>") {
    currentSource() = SourceManager(buf, std::string(name));
}

inline void clearSource() { currentSource() = SourceManager(); }

}  // namespace diag

// Source-located error. With a registered source buffer the output looks
// like clang:
//
//     [ERROR] shader.src:6:13: Expected ';' after assignment
//         float x = 1.0
//                     ^
//
// Without one, falls back to the original one-liner so cross-compiled
// runtime code keeps working. The location is an opaque offset; the
// SourceManager turns it into line/column and pulls the source line.
inline void logErrorAt(SourceLocation loc, const std::string& msg) {
    auto& sm = diag::currentSource();
    auto lc = sm.getLineCol(loc);
    if (!sm.hasBuffer() || lc.line <= 0) {
        logError(fmt::format("line {}, col {}: {}", lc.line, lc.col, msg));
        return;
    }
    auto src = sm.getLineText(loc);
    std::cerr << fmt::format("[ERROR] {}:{}:{}: {}\n", sm.name(), lc.line,
                             lc.col, msg);
    if (!src.empty()) {
        std::cerr << "    " << src << '\n';
        // Build the caret indent from the line prefix, preserving tabs so the
        // caret lines up under the offending column at the terminal's tab stops
        // (matching how the source line above is rendered).
        int prefix = lc.col > 0 ? lc.col - 1 : 0;
        std::string pad;
        for (int i = 0; i < prefix && i < static_cast<int>(src.size()); ++i)
            pad += (src[i] == '\t') ? '\t' : ' ';
        std::cerr << "    " << pad << "^\n";
    }
}

// Source-located format-string error — logErrorFmt + position.
template <typename... Args>
inline void logErrorFmtAt(SourceLocation loc, const std::string& fmtStr, Args&&... args) {
    logErrorAt(loc, fmt::format(fmt::runtime(fmtStr), std::forward<Args>(args)...));
}

#endif  // COMPILER_GLSL_ERROR_UTILS_FMT_H
