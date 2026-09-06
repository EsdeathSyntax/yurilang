#include <string>
#include <unordered_map>
#include <stdexcept>

#pragma once

namespace Yuri {

struct NativeSig {
    void* ptr;
    std::string ret_type;
    std::vector<std::string> param_types;
};

class Registry {
private:
    inline static std::unordered_map<std::string, NativeSig> native_functions;

public:
    static void register_native_fn(const std::string& name, void* ptr, const std::string& ret, const std::vector<std::string>& params) {
        if (!ptr) {
        Logger::log(Subsystem::Runtime, LogLevel::Warning, "Skipping registration for '" + name + "': resolved symbol pointer is null.");
        return;
    }
        native_functions[name] = {ptr, ret, params};
    }

    static const NativeSig* get_native_sig(const std::string& name) {
        auto it = native_functions.find(name);
        if (it != native_functions.end()) return &it->second;
        return nullptr;
    }

    static void* get_native_fn(const std::string& name) {
        auto it = native_functions.find(name);
        if (it != native_functions.end()) return it->second.ptr;
        return nullptr;
    }

    static std::unordered_map<std::string, void*> get_all_native_fns() {
        std::unordered_map<std::string, void*> simple_map;
        for (const auto& [name, sig] : native_functions) {
            simple_map[name] = sig.ptr;
        }
        return simple_map;
    }
};

} // namespace Yuri