#include "dev/pci.hpp"

#include "lib/port.hpp"
#include "lib/types.hpp"


namespace {

    constexpr u16 CONFIG_ADDR = 0xCF8;
    constexpr u16 CONFIG_DATA = 0xCFC;

    u32 make_addr(u8 bus, u8 dev, u8 func, u8 offset) {
        return 0x80000000 | (static_cast<u32>(bus) << 16)
                          | (static_cast<u32>(dev) << 11)
                          | (static_cast<u32>(func) << 8)
                          | (offset & 0xFC);
    }

}


namespace pci {

    u32 read_config(u8 bus, u8 dev, u8 func, u8 offset) {
        port::outl(CONFIG_ADDR, make_addr(bus, dev, func, offset));
        return port::inl(CONFIG_DATA);
    }

    void write_config(u8 bus, u8 dev, u8 func, u8 offset, u32 value) {
        port::outl(CONFIG_ADDR, make_addr(bus, dev, func, offset));
        port::outl(CONFIG_DATA, value);
    }

    u64 get_bar(u8 bus, u8 dev, u8 func, int bar_index) {
        u32 low = read_config(bus, dev, func, 0x10 + bar_index * 4);
        if ((low & 0x6) == 0x4) { // 64‑bit BAR: read the next dword too
            u32 high = read_config(bus, dev, func, 0x10 + (bar_index + 1) * 4);
            return (static_cast<u64>(high) << 32) | (low & ~0xFULL);
        }
        return low & ~0xFULL;
    }

    bool enumerate(bool (*callback)(const Device&)) {
        for (u16 bus = 0; bus < 256; ++bus) {
            for (u8 dev = 0; dev < 32; ++dev) {
                for (u8 func = 0; func < 8; ++func) {
                    u32 id = read_config(bus, dev, func, 0);
                    u16 vendor = id & 0xFFFF;
                    u16 device = id >> 16;
                    if (vendor == 0xFFFF) {
                        // no device on this function, skip the rest
                        if (func == 0) break;
                        continue;
                    }

                    u32 class_reg = read_config(bus, dev, func, 0x08);
                    Device d{};
                    d.bus       = bus;
                    d.device    = dev;
                    d.function  = func;
                    d.vendor_id = vendor;
                    d.device_id = device;
                    d.class_code = (class_reg >> 24) & 0xFF;
                    d.subclass   = (class_reg >> 16) & 0xFF;
                    d.prog_if    = (class_reg >> 8) & 0xFF;
                    for (int i = 0; i < 6; ++i) {
                        d.bar[i] = static_cast<u32>(get_bar(bus, dev, func, i));
                    }
                    if (callback(d)) return true;
                }
            }
        }
        return false;
    }

}
