#pragma once
#include <string>
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>

namespace Yuri {

enum class LogLevel {
    Info,
    Warning,
    Error,
    Debug
};

enum class Subsystem {
    Lexer,
    Parser,
    ModuleManager,
    Codegen,
    Runtime,
    General
};

class Logger {
    inline static std::mutex log_mutex;
    inline static std::string log_filepath = "yuri_log.txt";

    static std::string subsystem_to_string(Subsystem sub) {
        switch (sub) {
            case Subsystem::Lexer:         return "LEXER";
            case Subsystem::Parser:        return "PARSER";
            case Subsystem::ModuleManager: return "MODULE";
            case Subsystem::Codegen:       return "CODEGEN";
            case Subsystem::Runtime:       return "RUNTIME";
            case Subsystem::General:       return "GENERAL";
        }
        return "UNKNOWN";
    }

    static std::string level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::Info:    return "INFO";
            case LogLevel::Warning: return "WARN";
            case LogLevel::Error:   return "ERROR";
            case LogLevel::Debug:   return "DEBUG";
        }
        return "INFO";
    }

public:
    static void init(const std::string& path = "yuri_log.txt") {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_filepath = path;
        std::ofstream file(log_filepath, std::ios::trunc); // Fresh log per run
    }

    static void log(Subsystem sub, LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex);
        
        // Format timestamp
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
        std::string timestamp = ss.str();

        std::string formatted = "[" + timestamp + "][" + subsystem_to_string(sub) + "][" + level_to_string(level) + "] " + message;

        // Mirror to terminal
        if (level == LogLevel::Error) {
            std::cerr << formatted << "\n";
        } else {
            std::cout << formatted << "\n";
        }

        // Mirror to log file
        std::ofstream file(log_filepath, std::ios::app);
        if (file.is_open()) {
            file << formatted << "\n";
        }
    }
};

} // namespace Yuri