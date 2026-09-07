#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <xmmintrin.h>
#include <immintrin.h>
#include <bit>

namespace math {
    int64_t idiv(int64_t a, int64_t b) {
        if (b == 0) {
            throw std::runtime_error("Attempted to divide by zero.");
        }
        return a / b;
    }
    
    double sqrt(double val) {
        return std::sqrt(val);
    }
}