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
    uint64_t key_ptr;
    uint64_t val_bits;
};

struct TableHeader {
    int64_t tag;
    int64_t count;
    TableEntry entries[1]; // flexible array member
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
        void print_dynamic(uint64_t bits);

        void print_array(ArrayHeader* arr) {
            if (!arr) {
                std::cout << "[]";
                return;
            }
            std::cout << "[";
            for (uint64_t i = 0; i < arr->length; ++i) {
                print_dynamic(arr->data[i]);
                if (i + 1 < arr->length) std::cout << ", ";
            }
            std::cout << "]";
        }

        bool is_likely_string(uint64_t bits) {
            if (bits < 0x10000) return false;
            char* ptr = reinterpret_cast<char*>(bits);
            for (int i = 0; i < 256; ++i) {
                char c = ptr[i];
                if (c == '\0') return i > 0;
                if ((c < 32 && c != '\t' && c != '\n' && c != '\r') || c > 126) {
                    return false;
                }
            }
            return false;
        }

        void print_table(TableHeader* tbl) {
            if (!tbl) {
                std::cout << "{}";
                return;
            }
            std::cout << "{";
            for (uint64_t i = 0; i < tbl->count; ++i) {
                uint64_t key_bits = tbl->entries[i].key_ptr;
                std::cout << "\"" << reinterpret_cast<const char*>(key_bits) << "\": ";
                print_dynamic(tbl->entries[i].val_bits);
                if (i + 1 < tbl->count) std::cout << ", ";
            }
            std::cout << "}";
        }

        void print_dynamic(uint64_t bits) {
            uint64_t exponent = (bits >> 52) & 0x7FF;
            bool is_float = (exponent != 0 && exponent != 2047);
            bool is_potential_ptr = (bits >= 0x10000) && 
                                    ((bits & 0xFFF0000000000000ULL) == 0) && 
                                    (bits % alignof(int64_t) == 0);

            if (is_float) {
                print_value_tagged(bits, ValueType::Float);
            } else if (is_likely_string(bits)) {
                print_value_tagged(bits, ValueType::String);
            } else if (is_potential_ptr) {
                int64_t* header = reinterpret_cast<int64_t*>(bits);
                int64_t tag = header[0];
                if (tag == 1) {
                    print_value_tagged(bits, ValueType::Array);
                } else if (tag == 2) {
                    print_value_tagged(bits, ValueType::Table);
                } else {
                    print_value_tagged(bits, ValueType::Int);
                }
            } else {
                print_value_tagged(bits, ValueType::Int);
            }
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

    void write(int64_t bits) {
        print_dynamic(bits);
        std::cout << "\n";
    }
}