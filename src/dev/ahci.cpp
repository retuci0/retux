#include "dev/ahci.hpp"
#include "dev/pci.hpp"

#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

#include "lib/hex.hpp"
#include "io/serial.hpp"


namespace {

    // HBA register offsets (ABAR + offset)
    struct HBA {
        u32 cap;  // 0x00
        u32 ghc;  // 0x04
        u32 is;   // 0x08
        u32 pi;   // 0x0C
        u32 vs;   // 0x10
        u8  reserved0[0x100 - 0x14];
        u8  ports[0x100];  // each port has 0x80 bytes
    } __attribute__((packed));

    struct HBAPort {
        u32 clbl;       // 0x00 command list base (low)
        u32 clbh;       // 0x04 command list base (high)
        u32 fbl;        // 0x08 FIS base (low)
        u32 fbh;        // 0x0C FIS base (high)
        u32 is;         // 0x10 interrupt status
        u32 ie;         // 0x14 interrupt enable
        u32 cmd;        // 0x18 command & status
        u32 reserved0;  // 0x1C
        u32 tfd;        // 0x20 task file data
        u32 sig;        // 0x24 signature
        u32 ssts;       // 0x28 SATA status
        u32 sctl;       // 0x2C SATA control
        u32 serr;       // 0x30 SATA error
        u32 sact;       // 0x34 SATA active
        u32 ci;         // 0x38 command issue
        u32 sntf;       // 0x3C SATA notification
        u32 fbs;        // 0x40 FIS‑based switching
        u32 reserved1[15];
        u32 vendor[4];
    } __attribute__((packed));

    constexpr u32 GHC_AE = 1 << 31; // AHCI enable
    constexpr u32 GHC_HR = 1 << 0;  // HBA reset

    constexpr u32 CMD_ST = 1 << 0;   // start port
    constexpr u32 CMD_FRE = 1 << 4;  // FIS receive enable
    constexpr u32 CMD_CR = 1 << 15;  // command running
    constexpr u32 CMD_FR = 1 << 14;  // FIS running

    constexpr u32 TFD_BSY = 1 << 7;  // device busy
    constexpr u32 TFD_DRQ = 1 << 3;  // data request

    // command list entry (32 bytes each)
    struct CmdHeader {
        u32 dbal;      // data base address (low)
        u32 dbah;      // data base address (high)
        u16 prdtl;     // PRDT length
        u16 reserved;
        u32 reserved2;
        u64 ctu;       // command table address
    } __attribute__((packed));

    // command table (aligned to 128 bytes)
    struct CmdTable {
        u8 cfis[64];
        u8 acmd[16];
        u8 reserved[48];
        u8 prdt[0];    // physical region descriptor table
    } __attribute__((packed));

    // PRDT entry (16 bytes)
    struct PRDTEntry {
        u32 data_base;
        u32 data_base_high;
        u32 byte_count; // bits 21:0 = byte count, bit 22 = interrupt on complete
        u32 reserved;
    } __attribute__((packed));

    // ATA IDENTIFY command (0xEC)
    constexpr u8 ATA_IDENTIFY = 0xEC;

    volatile HBA* hba = nullptr;
    volatile HBAPort* port = nullptr;
    u8* identify_data = nullptr; // 512 bytes

    // physical addresses for the HBA's memory structures
    u64 cmd_list_phys = 0;
    u64 fis_phys = 0;
    u64 cmd_table_phys = 0;
    u64 identify_phys = 0;

    // virtual addresses for us to write to
    CmdHeader* cmd_list_virt = nullptr;
    u8* fis_virt = nullptr;
    CmdTable* cmd_table_virt = nullptr;

    void wait_ms(u32 ms) {
        for (u32 i = 0; i < ms * 1000; ++i) asm volatile("pause");
    }

    void port_wait_clear(volatile HBAPort* p, u32 mask) {
        for (u32 i = 0; i < 100000; ++i) {
            if (!(p->cmd & mask)) return;
            asm volatile("pause");
        }
    }

    bool init_port(u32 port_no) {
        port = reinterpret_cast<volatile HBAPort*>(
            reinterpret_cast<volatile u8*>(hba) + 0x100 + port_no * 0x80
        );

        // disable port
        port->cmd &= ~CMD_ST;
        port_wait_clear(port, CMD_CR);

        // allocate 1 page (4KB) for command list + FIS + command table + identify data
        u64 page_phys = pmm::alloc_frame();
        u64 page_virt = page_phys; // identity‑mapped for now
        vmm::map(page_virt, page_phys, vmm::KERNEL_RW);

        cmd_list_phys = page_phys;
        fis_phys      = page_phys + 0x400;  // 1KB
        cmd_table_phys= page_phys + 0x500;  // +256 bytes for FIS = 0x500
        identify_phys = page_phys + 0x600;  // +256 for command table = 0x600 (512 bytes)

        cmd_list_virt  = reinterpret_cast<CmdHeader*>(page_virt);
        fis_virt       = reinterpret_cast<u8*>(page_virt + 0x400);
        cmd_table_virt = reinterpret_cast<CmdTable*>(page_virt + 0x500);
        identify_data  = reinterpret_cast<u8*>(page_virt + 0x600);

        // zero them
        for (u16 i = 0; i < 4096; ++i) {
            reinterpret_cast<u8*>(page_virt)[i] = 0;
        }

        // set up command list: 1 entry
        port->clbl = cmd_list_phys & 0xFFFFFFFF;
        port->clbh = 0;

        // set up FIS receive area
        port->fbl  = fis_phys & 0xFFFFFFFF;
        port->fbh  = 0;

        // clear error & interrupt status
        port->is   = ~0;
        port->serr = ~0;

        // enable FIS receive
        port->cmd |= CMD_FRE;
        wait_ms(1);

        // start port
        port->cmd |= CMD_ST;
        wait_ms(1);

        // check if device present
        u32 sig = port->sig;
        if ((sig >> 16) != 0xEB14 && (sig >> 16) != 0x9669) {
            serial::print("ahci: port ");
            char buf[17]; hex::to_string(port_no, buf); serial::print(buf);
            serial::print(" no ATA device\n");
            return false;
        }

        return true;
    }

    void send_identify() {
        // build the CFIS for IDENTIFY
        u8* cfis = cmd_table_virt->cfis;
        cfis[0] = 0x27;          // CFIS type: register FIS
        cfis[1] = 0x80;          // command
        cfis[2] = ATA_IDENTIFY;  // command code
        cfis[4] = 0x00;          // features low
        cfis[7] = 0x00;          // device head
        cfis[12]= 0x00;          // features high

        // PRDT: one entry pointing to the 512‑byte buffer
        PRDTEntry* prdt = reinterpret_cast<PRDTEntry*>(cmd_table_virt->prdt);
        prdt->data_base      = identify_phys & 0xFFFFFFFF;
        prdt->data_base_high = 0;
        prdt->byte_count     = 511; // 0‑based: 511 means 512 bytes
        prdt->reserved       = 0;

        // command header
        cmd_list_virt[0].dbal  = identify_phys & 0xFFFFFFFF;
        cmd_list_virt[0].dbah  = 0;
        cmd_list_virt[0].prdtl = 1;  // one PRDT entry
        cmd_list_virt[0].ctu   = cmd_table_phys;

        // issue command
        port->ci = 1; // issue slot 0

        // wait for completion
        for (u32 i = 0; i < 1000000; ++i) {
            if (port->ci == 0) break;
            asm volatile("pause");
        }

        if (port->is & (1 << 30)) {
            serial::print("ahci: identify command failed (device error)\n");
            return;
        }

        // extract model name (bytes 27..46, 20 bytes, ASCII, padded with spaces)
        char model[21];
        for (u8 i = 0; i < 20; ++i) {
            model[i] = identify_data[27 + i + 1]; // ATA uses 16‑bit words, so swap bytes
        }
        model[20] = '\0';

        // trim trailing spaces
        for (u32 i = 19; i >= 0 && model[i] == ' '; --i) model[i] = '\0';

        serial::print("ahci: drive model: ");
        serial::print(model);
        serial::print("\n");
    }

}


namespace ahci {

    void init() {
        // find AHCI controller: class 0x01 (mass storage), subclass 0x06 (SATA), progIF 0x01 (AHCI)
        bool found = pci::enumerate([](const pci::Device& d) {
            if (d.class_code == 0x01 && d.subclass == 0x06 && d.prog_if == 0x01) {
                serial::print("ahci: found AHCI controller at ");
                char buf[17];
                hex::to_string(d.bus, buf); serial::print(buf); serial::print(":");
                hex::to_string(d.device, buf); serial::print(buf); serial::print(".");
                hex::to_string(d.function, buf); serial::print(buf); serial::print("\n");

                u64 abar = pci::get_bar(d.bus, d.device, d.function, 5);
                serial::print("ahci: ABAR = 0x");
                hex::to_string(abar, buf); serial::print(buf); serial::print("\n");

                vmm::map(abar, abar, vmm::KERNEL_RW | vmm::PWT | vmm::PCD);
                hba = reinterpret_cast<volatile HBA*>(abar);

                // enable AHCI
                hba->ghc |= GHC_AE;
                wait_ms(10);
                hba->ghc |= GHC_HR;   // reset
                wait_ms(100);
                hba->ghc &= ~GHC_HR;  // clear reset
                wait_ms(10);
                hba->ghc |= GHC_AE;

                // find first implemented port
                u32 ports_impl = hba->pi;
                for (u8 i = 0; i < 32; ++i) {
                    if (ports_impl & (1 << i)) {
                        serial::print("ahci: initialising port ");
                        char buf[17]; hex::to_string(i, buf); serial::print(buf);
                        serial::print("\n");
                        if (init_port(i)) {
                            send_identify();
                        }
                        break;  // only first drive for now
                    }
                }
                return true;  // stop enumeration
            }
            return false;
        });

        if (!found) {
            serial::print("ahci: no AHCI controller found\n");
        }
    }

}
