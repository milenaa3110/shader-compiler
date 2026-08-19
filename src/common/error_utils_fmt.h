// error_utils_fmt.h — format-string error logger, depends on libfmt.
// Included by host-compiled translation units that link fmt::fmt.
// Cross-compiled (RISC-V) sources take plain "error_utils.h" instead.
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

// Source buffer used for caret diagnostics.
// Registered before parsing and cleared afterwards.
// Without a buffer, diagnostics fall back to plain text.

namespace diag {

inline SourceManager& currentSource() {
    static SourceManager sm;
    return sm;
}

inline void setSource(std::string_view buf, std::string_view name = "<stdin>") {
    currentSource() = SourceManager(buf, std::string(name));
}

inline void clearSource() { currentSource() = SourceManager(); }

} // namespace diag

// Source-located error. With a registered source buffer, prints the
// file name, line, column, source line, and a caret pointing to the error.
// Without a source buffer, falls back to a one-line diagnostic.
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
