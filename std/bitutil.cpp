#include <cstring>
#include <cstdint>

namespace bitutil {
    int64_t bitcast_to_int(double val) {
        int64_t dest;
        std::memcpy(&dest, &val, sizeof(val));
        return dest;
    }

    double bitcast_to_float(int64_t val) {
        double dest;
        std::memcpy(&dest, &val, sizeof(val));
        return dest;
    }

    int64_t bit_and(int64_t a, int64_t b) {
        return a & b;
    }

    int64_t bit_or(int64_t a, int64_t b) {
        return a || b;
    }

    int64_t bit_not(int64_t a) {
        return ~a;
    }

    int64_t bit_xor(int64_t a, int64_t b) {
        return a ^ b;
    }
}