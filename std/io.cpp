#include <iostream>
#include <cstdint>
#include <string>
#include <cstring>
#include <unordered_map>

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
    TableEntry entries[1];
};

namespace io {
    namespace {
        bool is_likely_string(uint64_t bits) {
            if (bits < 0x10000) return false;
            char* ptr = reinterpret_cast<char*>(bits);
            for (int i = 0; i < 64; ++i) {
                if (ptr[i] == '\0') return i > 0;
                if (ptr[i] < 32 || ptr[i] > 126) return false;
            }
            return false;
        }

        bool is_likely_array_or_table(uint64_t bits) {
            if (bits < 0x10000) return false;
            uint64_t* ptr = reinterpret_cast<uint64_t*>(bits);
            return ptr[0] < 100000 && ptr[1] < 100000;
        }

        void yuri_print_val(uint64_t bits, bool is_float_hint = false) {
            if (is_float_hint) {
                double dval;
                std::memcpy(&dval, &bits, 8);
                std::cout << dval;
                return;
            }
            if (is_likely_string(bits)) {
                std::cout << "\"" << reinterpret_cast<char*>(bits) << "\"";
                return;
            }
            if (is_likely_array_or_table(bits)) {
                auto* arr = reinterpret_cast<ArrayHeader*>(bits);
                std::cout << "[";
                for (uint64_t i = 0; i < arr->length; ++i) {
                    yuri_print_val(arr->data[i]);
                    if (i + 1 < arr->length) std::cout << ", ";
                }
                std::cout << "]";
                return;
            }

            std::cout << static_cast<int64_t>(bits);
        }
    }

    void write(const char* str) {
        if (str) {
            std::cout << str << "\n";
        } else {
            std::cout << "null\n";
        }
    }

    void write(int64_t bits) {
        yuri_print_val(static_cast<uint64_t>(bits));
        std::cout << "\n";
    }

    void write(double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, 8);
        yuri_print_val(bits, true);
        std::cout << "\n";
    }

    void write(void* ptr) {
        yuri_print_val(reinterpret_cast<uint64_t>(ptr));
        std::cout << "\n";
    }

    extern "C" char* yuri_str_concat(const char* a, const char* b) {
        if (!a) a = "";
        if (!b) b = "";
        size_t len_a = std::strlen(a);
        size_t len_b = std::strlen(b);
        char* res = static_cast<char*>(std::malloc(len_a + len_b + 1));
        std::memcpy(res, a, len_a);
        std::memcpy(res + len_a, b, len_b);
        res[len_a + len_b] = '\0';
        return res;
    }
}