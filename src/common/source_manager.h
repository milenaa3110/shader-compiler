// source_manager.h — source locations as byte offsets, plus a SourceManager
// that reconstructs (line, column) and the source line on demand.
//
// Modelled on LLVM's SMLoc / SourceMgr: a SourceLocation is an opaque offset
// into the source buffer, nothing more. The lexer stamps tokens with offsets
// (trivially — just `Cur - Begin`) and never counts lines or columns; the
// translation to human-readable positions happens lazily, only when a
// diagnostic is actually printed. That keeps Token small, makes advance()
// branch-free, and moves all the \r / \n / tab bookkeeping into one place.
#ifndef COMPILER_GLSL_SOURCE_MANAGER_H
#define COMPILER_GLSL_SOURCE_MANAGER_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// An opaque position in the source buffer (a byte offset). Default-constructed
// locations are invalid — that is the "unstamped" signal a parse path that
// forgot to record a position leaves behind.
struct SourceLocation {
  static constexpr uint32_t Invalid = ~uint32_t{0};
  uint32_t Offset = Invalid;

  bool isValid() const { return Offset != Invalid; }
  static SourceLocation fromOffset(uint32_t off) { return SourceLocation{off}; }
};

// Owns (a view of) one source buffer and answers position queries about it.
// Line starts are computed once, lazily, on the first query.
class SourceManager {
 public:
  SourceManager() = default;
  SourceManager(std::string_view buffer, std::string name)
      : Buffer(buffer), Name(std::move(name)) {}

  bool hasBuffer() const { return !Buffer.empty(); }
  const std::string& name() const { return Name; }

  struct LineCol {
    int line = 0;  // 1-based; 0 == invalid/unknown
    int col = 0;   // 1-based byte column within the line
  };

  // Map an offset to a 1-based (line, column). Returns {0, 0} for an invalid
  // location or one past the end of the buffer.
  LineCol getLineCol(SourceLocation loc) const {
    if (!loc.isValid() || loc.Offset > Buffer.size()) return {};
    ensureLineStarts();
    // First line whose start is strictly past the offset; the offset belongs to
    // the line right before it.
    auto it = std::upper_bound(LineStarts.begin(), LineStarts.end(), loc.Offset);
    int line = static_cast<int>(it - LineStarts.begin());  // 1-based
    size_t lineStart = LineStarts[static_cast<size_t>(line) - 1];
    int col = static_cast<int>(loc.Offset - lineStart) + 1;
    return {line, col};
  }

  // The text of the source line containing `loc` (without the line terminator),
  // for caret rendering. Empty if the location is invalid.
  std::string_view getLineText(SourceLocation loc) const {
    if (!loc.isValid() || Buffer.empty()) return {};
    size_t off = std::min<size_t>(loc.Offset, Buffer.size());
    size_t start = off;
    while (start > 0 && Buffer[start - 1] != '\n' && Buffer[start - 1] != '\r')
      --start;
    size_t end = off;
    while (end < Buffer.size() && Buffer[end] != '\n' && Buffer[end] != '\r')
      ++end;
    return Buffer.substr(start, end - start);
  }

 private:
  std::string_view Buffer;
  std::string Name = "<stdin>";
  mutable std::vector<size_t> LineStarts;  // offset of each line's first byte

  void ensureLineStarts() const {
    if (!LineStarts.empty()) return;
    LineStarts.push_back(0);
    for (size_t i = 0; i < Buffer.size(); ++i) {
      char c = Buffer[i];
      if (c == '\n') {
        LineStarts.push_back(i + 1);
      } else if (c == '\r') {
        // Treat \r\n as a single break: step over the \n so it doesn't start
        // its own empty line.
        if (i + 1 < Buffer.size() && Buffer[i + 1] == '\n') ++i;
        LineStarts.push_back(i + 1);
      }
    }
  }
};

#endif  // COMPILER_GLSL_SOURCE_MANAGER_H
