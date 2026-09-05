#pragma once
#include <string>
#include <memory>
#include <vector>

namespace Yuri {

enum class TypeKind {
    Void,
    Int64,
    Int32,
    Bool,
    Float32,
    Float64,
    String,
    Array,
    Table,
    Pointer,
    Custom
};

class Type {
public:
    TypeKind kind;
    std::string name; // For custom structs or detailed names
    std::shared_ptr<Type> inner_type; // For arrays (e.g., Array<float>) or pointers

    static std::shared_ptr<Type> make(TypeKind k, const std::string& n = "") {
        auto t = std::make_shared<Type>();
        t->kind = k;
        t->name = n;
        return t;
    }

    static std::shared_ptr<Type> make_array(std::shared_ptr<Type> elem_type) {
        auto t = std::make_shared<Type>();
        t->kind = TypeKind::Array;
        t->inner_type = elem_type;
        return t;
    }
};

} // namespace Yuri