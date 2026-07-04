#pragma once

#include "lib/types.hpp"


namespace keyboard {

    void init();

    // (non‑blocking) returns the next ASCII char from the buffer, or 0 if none.
    char getchar();

}
