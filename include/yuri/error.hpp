#pragma once
#include <string>

namespace Yuri {

class ErrorReporter {
public:
    inline static bool has_error = false;
    inline static std::string current_file = "unknown";

    static void set_file(const std::string& filepath) {
        current_file = filepath;
    }

    static void error(size_t line, size_t column, const std::string& message) {
        std::fprintf(stderr, "[%s:%zu:%zu] Error: %s\n", current_file.c_str(), line, column, message.c_str());
        has_error = true;
    }
};

} // namespace Yuri