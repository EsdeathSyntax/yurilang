#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <filesystem>
#include "yuri/lexer.hpp"
#include "yuri/parser.hpp"
#include "yuri/error.hpp"
#include "yuri/modulemanager.hpp"
#include "yuri/codegen.hpp"
#include "yuri/logger.hpp"
#include "yuri/registry.hpp"


void log_message(Yuri::Subsystem sub, Yuri::LogLevel level, const std::string& message) {
    Yuri::Logger::log(sub, level, message);
}


int main(int argc, char* argv[]) {
    Yuri::Logger::init("yuri_log.txt");
    log_message(Yuri::Subsystem::General, Yuri::LogLevel::Info, "Starting YuriLang compiler pipeline.");

    if (argc < 2) {
        log_message(Yuri::Subsystem::General, Yuri::LogLevel::Error, "Usage: yurilang <source_file_or_directory>");
        return 1;
    }
    Yuri::Logger::log(Yuri::Subsystem::General, Yuri::LogLevel::Debug, argv[0]);
    std::string input_path = argv[1];

    std::vector<std::string> program_args;
    for (int i = 2; i < argc; ++i) {
        program_args.push_back(argv[i]);
    }

    try {
        std::vector<Yuri::AST::Program*> programs;
        Yuri::ModuleManager mod_mgr(std::filesystem::is_directory(input_path) ? input_path : (std::filesystem::path(input_path).parent_path().empty() ? "." : std::filesystem::path(input_path).parent_path().string()));

        if (std::filesystem::is_directory(input_path)) {
            log_message(Yuri::Subsystem::ModuleManager, Yuri::LogLevel::Info, "Compiling directory: " + input_path);
            programs = mod_mgr.compile_directory(input_path);
        } else {
            log_message(Yuri::Subsystem::ModuleManager, Yuri::LogLevel::Info, "Compiling file: " + input_path);
            std::filesystem::path p(input_path);
            std::string mod_name = p.stem().string();
            
            programs.push_back(mod_mgr.load_module(mod_name));
        }

        if (Yuri::ErrorReporter::has_error) {
            log_message(Yuri::Subsystem::General, Yuri::LogLevel::Error, "Build aborted due to logged errors.");
            return 1;
        }

        for (size_t i = 0; i < programs.size(); ++i) {
            if (!programs[i]) {
                log_message(Yuri::Subsystem::General, Yuri::LogLevel::Error, "Failed to parse program AST index " + std::to_string(i));
                return 1;
            }
        }

        log_message(Yuri::Subsystem::ModuleManager, Yuri::LogLevel::Info, "Parsing successful! Processing code generation across " + std::to_string(programs.size()) + " module(s).");

        Yuri::CodeGenerator codegen;
        for (auto* prog : programs) {
            codegen.compile(prog);
        }

        log_message(Yuri::Subsystem::Codegen, Yuri::LogLevel::Info, "Triggering JIT execution engine...");
        codegen.exec(program_args);

    } catch (const std::exception& e) {
        log_message(Yuri::Subsystem::General, Yuri::LogLevel::Error, std::string("Fatal Error: ") + e.what());
        return 1;
    }

    return 0;
}