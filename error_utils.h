// error_utils.h — minimal error logger usable in any TU (no fmt dependency).
// For format-string variants, include "error_utils_fmt.h" instead, which
// transitively includes this header and adds logErrorFmt / logErrorContext.
#ifndef COMPILER_GLSL_ERROR_UTILS_H
#define COMPILER_GLSL_ERROR_UTILS_H

#include <iostream>
#include <string>

inline void logError(const char* msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

inline void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

#endif  // COMPILER_GLSL_ERROR_UTILS_H
