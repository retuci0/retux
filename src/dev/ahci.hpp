#pragma once

#include "lib/types.hpp"


namespace ahci {

    // scan PCI, find an AHCI controller, initialise it and the first drive.
    // prints the model name via serial + VGA.
    void init();

    // [stub for future] read one sector.
    // bool read_sectors(u32 lba, u32 count, void* buffer);

}
