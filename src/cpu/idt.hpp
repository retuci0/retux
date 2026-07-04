#pragma once

#include "lib/types.hpp"


namespace idt {

    void init();

    // install a single interrupt gate (ist=0) pointing at `handler`.
    // exposed so other subsystems can fill in vectors idt::init()
    // deliberately leaves empty (32-255) - currently just irq::init(),
    // which uses this for vectors 32-47.
    void install_gate(u64 vector, u64 handler);

}
