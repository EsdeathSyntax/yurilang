#include <iostream>
#include <cstdint>
#include <string>
#include <cstring>

struct ArrayHeader {
    uint64_t capacity;
    uint64_t length;
    uint64_t data[1];
};

struct TableEntry {
    uint64_t key_hash;
    uint64_t val_bits;
};

struct TableHeader {
    uint64_t count;
    uint64_t capacity;
    TableEntry entries[1];
};

enum class ValueType : uint8_t {
    Int = 1,
    Float = 2,
    Bool = 3,
    String = 4,
    Array = 5,
    Table = 6,
    Nil = 7
};

namespace io {
    namespace {
        void print_value_tagged(uint64_t bits, ValueType type);

        void print_array(ArrayHeader* arr) {
            if (!arr) {
                std::cout << "[]";
                return;
            }
            std::cout << "[";
            for (uint64_t i = 0; i < arr->length; ++i) {
                print_value_tagged(arr->data[i], ValueType::Int); 
                if (i + 1 < arr->length) std::cout << ", ";
            }
            std::cout << "]";
        }

        void print_table(TableHeader* tbl) {
            if (!tbl) {
                std::cout << "{}";
                return;
            }
            std::cout << "{";
            for (uint64_t i = 0; i < tbl->count; ++i) {
                std::cout << "\"" << tbl->entries[i].key_hash << "\": ";
                print_value_tagged(tbl->entries[i].val_bits, ValueType::Int);
                if (i + 1 < tbl->count) std::cout << ", ";
            }
            std::cout << "}";
        }

        bool is_likely_string(uint64_t bits) {
            if (bits < 0x10000) return false;
            char* ptr = reinterpret_cast<char*>(bits);
            for (int i = 0; i < 64; ++i) {
                if (ptr[i] == '\0') return i > 0;
                if (ptr[i] < 32 || ptr[i] > 126) return false;
            }
            return false;
        }

        void print_value_tagged(uint64_t bits, ValueType type) {
            switch (type) {
                case ValueType::Int:
                    std::cout << static_cast<int64_t>(bits);
                    break;
                case ValueType::Float: {
                    double dval;
                    std::memcpy(&dval, &bits, 8);
                    std::cout << dval;
                    break;
                }
                case ValueType::Bool:
                    std::cout << (bits ? "true" : "false");
                    break;
                case ValueType::String:
                    std::cout << "\"" << reinterpret_cast<char*>(bits) << "\"";
                    break;
                case ValueType::Array:
                    print_array(reinterpret_cast<ArrayHeader*>(bits));
                    break;
                case ValueType::Table:
                    print_table(reinterpret_cast<TableHeader*>(bits));
                    break;
                case ValueType::Nil:
                    std::cout << "null";
                    break;
                default:
                    std::cout << "null";
                    break;
            }
        }
    }

    // Single unified write function for all types and payloads
    void write(int64_t bits) {
        // Safe automatic heuristic fallback if untagged bits are passed
        if (is_likely_string(bits)) {
            print_value_tagged(bits, ValueType::String);
        } else if (bits >= 0x10000 && (bits % alignof(int64_t) == 0)) {
            auto* arr = reinterpret_cast<ArrayHeader*>(bits);
            if (arr->length < 10000000 && arr->capacity >= arr->length) {
                print_value_tagged(bits, ValueType::Array);
            } else {
                print_value_tagged(bits, ValueType::Int);
            }
        } else {
            print_value_tagged(bits, ValueType::Int);
        }
        std::cout << "\n";
    }


}