#include <cmath>
#include <stdexcept>
#include <cstdint>

namespace math {
    int64_t idiv(int64_t a, int64_t b) {
        if (a == 0 || b == 0) {
            throw std::runtime_error("Attempted to divide with zero.");
        }
        return a / b;
    }
    
    float sqrt(float x) {
        return std::sqrt(x);
    }
}