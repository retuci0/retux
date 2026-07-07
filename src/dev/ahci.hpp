#pragma once

#include "lib/types.hpp"


namespace ahci {

    // scan PCI, find an AHCI controller, initialise it and the first drive.
    // prints the model name via serial + VGA.
    void init();

    // read sectors or sum
    bool read_sectors(u64 lba, u32 count, void* buffer);
}
