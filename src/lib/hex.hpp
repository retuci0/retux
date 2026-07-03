#pragma once

#include "lib/types.hpp"


namespace hex {

    // at least 17 bytes (16 hex digits + null terminator)
    inline void to_string(u64 value, char out[17]) {
        constexpr char digits[] = "0123456789ABCDEF";
        out[16] = '\0';
        for (int i = 15; i >= 0; --i) {
            out[i] = digits[value & 0xF];
            value >>= 4;
        }
    }

}
