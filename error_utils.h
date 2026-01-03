// error_utils.h
#ifndef COMPILER_GLSL_ERROR_UTILS_H
#define COMPILER_GLSL_ERROR_UTILS_H

#include <iostream>
#include <string>
#include <fmt/format.h>

inline void logError(const char* msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

inline void logError(const std::string& msg) {
    std::cerr << "[ERROR] " << msg << std::endl;
}

template<typename... Args>
void logErrorFmt(const std::string& fmt, Args&&... args) {
    logError(fmt::format(fmt, std::forward<Args>(args)...));
}

inline void logErrorContext(const std::string& context, const std::string& msg) {
    logError(fmt::format("{}: {}", context, msg));
}

#endif //COMPILER_GLSL_ERROR_UTILS_H
