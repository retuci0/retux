#pragma once

#include "lib/types.hpp"


namespace pci {

    struct Device {
        u8  bus;
        u8  device;
        u8  function;
        u16 vendor_id;
        u16 device_id;
        u8  class_code;
        u8  subclass;
        u8  prog_if;
        u32 bar[6];
    };

    // scan all buses, devices, functions; calls `callback` for each valid device.
    // returns true if the callback returns true early, otherwise false.
    bool enumerate(bool (*callback)(const Device&));

    // read a 32‑bit config register for the given address.
    u32 read_config(u8 bus, u8 dev, u8 func, u8 offset);

    // write a 32‑bit config register
    void write_config(u8 bus, u8 dev, u8 func, u8 offset, u32 value);

    // get the BAR value (handles 64‑bit BARs by reading the next one too).
    u64 get_bar(u8 bus, u8 dev, u8 func, int bar_index);

}
