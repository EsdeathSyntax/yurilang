#include "yuri/modulemanager.hpp"
#include "yuri/lexer.hpp"
#include "yuri/parser.hpp"
#include "yuri/logger.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <dlfcn.h>
#include "yuri/registry.hpp"

#include "yuri/auto_symbols.hpp"

namespace Yuri {

ModuleManager::ModuleManager(std::string root_path) : root_directory(std::move(root_path)) {
    std::string std_path = "std";
    if (std::filesystem::exists(std_path) && std::filesystem::is_directory(std_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(std_path)) {
            if (entry.path().extension() == ".so") {
                std::string lib_path = entry.path().string();
                std::string file_stem = entry.path().stem().string();

                Logger::log(Subsystem::ModuleManager, LogLevel::Info, "Automatically loading and binding C++ module: " + file_stem);

                // Automatically load the shared library handle
                void* handle = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
                if (!handle) {
                    throw std::runtime_error("Failed to load module library: " + std::string(dlerror()));
                }

                // Automatically register all symbols with their scraped types using the updated auto-generated binder
                register_auto_symbols([](const char* name, void* ptr, const char* ret_type, const std::vector<std::string>& params) {
                    Registry::register_native_fn(name, ptr, ret_type, params);
                }, handle);

                loaded_cxx_modules[file_stem] = handle;
            }
        }
    }
}

AST::Program* ModuleManager::load_module(const std::string& mod_name) {
    if (loaded_modules.find(mod_name) != loaded_modules.end()) {
        return loaded_modules[mod_name].get();
    }

    std::string filepath = find_module_file(mod_name);
    if (filepath.empty()) {
        throw std::runtime_error("Module not found: " + mod_name);
    }

    std::string source = read_file(filepath);
    Lexer lexer(source);
    auto tokens = lexer.scan_tokens();

    Parser parser(std::move(tokens), filepath);
    auto program = parser.parse();

    std::string registered_name = program->module_name.empty() ? mod_name : program->module_name;
    AST::Program* ptr = program.get();
    loaded_modules[registered_name] = std::move(program);
    return ptr;
}

std::vector<AST::Program*> ModuleManager::compile_directory(const std::string& dir_path) {
    std::vector<AST::Program*> programs;
    
    std::string target_dir = dir_path.empty() ? root_directory : dir_path;
    if (!std::filesystem::exists(target_dir)) {
        throw std::runtime_error("Directory does not exist: " + target_dir);
    }

    // Ensure standard library modules are included first if present
    std::string std_dir = "std";
    if (std::filesystem::exists(std_dir) && std::filesystem::is_directory(std_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(std_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".yuri") {
                std::string filepath = entry.path().string();
                std::string file_stem = entry.path().stem().string();
                if (loaded_modules.find(file_stem) == loaded_modules.end()) {
                    programs.push_back(load_module(file_stem));
                }
            }
        }
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(target_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yuri") {
            std::string filepath = entry.path().string();
            std::string file_stem = entry.path().stem().string();

            // Skip files already loaded from std/ to avoid duplication
            if (entry.path().parent_path() == std::filesystem::path("std")) {
                continue;
            }

            std::string source = read_file(filepath);
            Lexer lexer(source);
            auto tokens = lexer.scan_tokens();
            
            Parser parser(std::move(tokens), filepath);
            auto program = parser.parse();

            std::string mod_name = program->module_name.empty() ? file_stem : program->module_name;

            if (loaded_modules.find(mod_name) == loaded_modules.end() || loaded_modules[mod_name] != nullptr) {
                programs.push_back(program.get());
                loaded_modules[mod_name] = std::move(program);
            } else {
                programs.push_back(loaded_modules[mod_name].get());
            }
        }
    }
    return programs;
}

std::vector<AST::Program*> ModuleManager::get_all_modules() const {
    std::vector<AST::Program*> programs;
    for (const auto& [name, prog] : loaded_modules) {
        if (prog != nullptr) {
            programs.push_back(prog.get());
        }
    }
    return programs;
}

std::string ModuleManager::find_module_file(const std::string& mod_name) {
    std::string search_paths[] = {"std", root_directory};
    for (const auto& base_path : search_paths) {
        if (!std::filesystem::exists(base_path)) continue;
        
        // Check direct path first (e.g. std/bitmath.yuri)
        std::string direct_path = base_path + "/" + mod_name + ".yuri";
        if (std::filesystem::exists(direct_path)) {
            return direct_path;
        }

        // Otherwise recursive search
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".yuri") {
                if (entry.path().stem().string() == mod_name) {
                    return entry.path().string();
                }
            }
        }
    }
    return "";
}

} // namespace Yuri