#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include "yuri/ast.hpp"

namespace Yuri {

class ModuleManager {
    std::unordered_map<std::string, std::unique_ptr<AST::Program>> loaded_modules;
    std::unordered_map<std::string, void*> loaded_cxx_modules;
    std::string root_directory;

public:
    explicit ModuleManager(std::string root_path);

    AST::Program* load_module(const std::string& mod_name);

    std::vector<AST::Program*> compile_directory(const std::string& dir_path);

    std::vector<AST::Program*> get_all_modules() const;

private:
    std::string find_module_file(const std::string& mod_name);
};

} // namespace Yuri