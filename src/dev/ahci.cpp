#include "dev/ahci.hpp"
#include "dev/pci.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"

#include "lib/hex.hpp"
#include "io/serial.hpp"


namespace {

    struct HBA {
        u32 cap;
        u32 ghc;
        u32 is;
        u32 pi;
        u32 vs;
        u8  reserved0[0x100 - 0x14];
        u8  ports[0x100];
    } __attribute__((packed));

    struct HBAPort {
        u32 clbl; u32 clbh;
        u32 fbl;  u32 fbh;
        u32 is;   u32 ie;
        u32 cmd;  u32 reserved0;
        u32 tfd;  u32 sig;
        u32 ssts; u32 sctl;
        u32 serr; u32 sact;
        u32 ci;   u32 sntf;
        u32 fbs;
        u32 reserved1[15];
        u32 vendor[4];
    } __attribute__((packed));

    constexpr u32 GHC_AE  = 1 << 31;
    constexpr u32 GHC_HR  = 1 << 0;
    constexpr u32 CMD_ST  = 1 << 0;
    constexpr u32 CMD_FRE = 1 << 4;
    constexpr u32 CMD_CR  = 1 << 15;

    // AHCI spec §4.2.2 - command list entry, 32 bytes.
    // field names and offsets match the spec exactly.
    struct CmdHeader {
        u16 opts;        // [4:0]=CFL (FIS length in DWORDs), [6]=W (write)
        u16 prdtl;       // PRDT entry count
        u32 prdbc;       // bytes transferred, filled by HBA
        u32 ctba;        // command table base address (128-byte aligned), low 32
        u32 ctbau;       // command table base address, upper 32
        u32 reserved[4];
    } __attribute__((packed));

    // AHCI spec §4.2.3.3 - physical region descriptor table entry, 16 bytes.
    struct PRDTEntry {
        u32 dbal;        // data base address, lower 32
        u32 dbau;        // data base address, upper 32
        u32 reserved;
        u32 dbc;         // byte count minus 1 (bits [21:0]); bit [31] = interrupt on completion
    } __attribute__((packed));

    struct CmdTable {
        u8 cfis[64];
        u8 acmd[16];
        u8 reserved[48];
        u8 prdt[0];
    } __attribute__((packed));

    constexpr u8 ATA_IDENTIFY    = 0xEC;
    constexpr u8 ATA_READ_DMA_EX = 0x25;

    volatile HBA* hba = nullptr;
    volatile HBAPort* sata_port = nullptr;
    u8* identify_data = nullptr;

    u64 cmd_list_phys  = 0;
    u64 fis_phys       = 0;
    u64 cmd_table_phys = 0;
    u64 identify_phys  = 0;

    CmdHeader* cmd_list_virt  = nullptr;
    u8*        fis_virt       = nullptr;
    CmdTable*  cmd_table_virt = nullptr;

    void wait_ms(u32 ms) {
        for (u32 i = 0; i < ms * 1000; ++i) asm volatile("pause");
    }

    void port_wait_clear(volatile HBAPort* p, u32 mask) {
        for (u32 i = 0; i < 100000; ++i) {
            if (!(p->cmd & mask)) return;
            asm volatile("pause");
        }
    }

    // --- command issue helpers ---

    // fill CmdHeader slot 0 to point at the static command table
    void setup_cmd_header(u16 opts, u16 prdtl) {
        cmd_list_virt[0].opts  = opts;
        cmd_list_virt[0].prdtl = prdtl;
        cmd_list_virt[0].prdbc = 0;
        cmd_list_virt[0].ctba  = cmd_table_phys & 0xFFFFFFFF;
        cmd_list_virt[0].ctbau = 0;
    }

    // build the single PRDT entry and fill cfis[0..15] for a 28-/48-bit read or identify.
    // for identify: lba=0, count=0, cmd=ATA_IDENTIFY, buf_phys=identify_phys, bytes=512.
    void setup_cfis_and_prdt(u8 ata_cmd, u64 lba, u32 sector_count, u64 buf_phys, u32 bytes) {
        u8* cfis = cmd_table_virt->cfis;
        for (int i = 0; i < 64; ++i) cfis[i] = 0;

        cfis[0]  = 0x27;                        // H2D register FIS
        cfis[1]  = 0x80;                        // command bit
        cfis[2]  = ata_cmd;
        cfis[3]  = 0;                           // features low
        cfis[4]  =  lba         & 0xFF;         // LBA 7:0
        cfis[5]  = (lba >>  8)  & 0xFF;         // LBA 15:8
        cfis[6]  = (lba >> 16)  & 0xFF;         // LBA 23:16
        cfis[7]  = 0x40;                        // device: LBA mode, device 0
        cfis[8]  = (lba >> 24)  & 0xFF;         // LBA 31:24
        cfis[9]  = (lba >> 32)  & 0xFF;         // LBA 39:32
        cfis[10] = (lba >> 40)  & 0xFF;         // LBA 47:40
        cfis[11] = 0;                           // features high
        cfis[12] =  sector_count       & 0xFF;  // count low
        cfis[13] = (sector_count >> 8) & 0xFF;  // count high

        PRDTEntry* prdt = reinterpret_cast<PRDTEntry*>(cmd_table_virt->prdt);
        prdt->dbal     = buf_phys & 0xFFFFFFFF;
        prdt->dbau     = 0;
        prdt->reserved = 0;
        prdt->dbc      = bytes - 1;
    }

    // issue slot 0 and wait; returns true on success
    bool issue_and_wait() {
        sata_port->is = ~0u;
        sata_port->ci = 1;

        u32 timeout = 5000000;
        while ((sata_port->ci & 1) && timeout--) asm volatile("pause");

        bool ok = (timeout > 0) && !(sata_port->is & (1u << 30));
        sata_port->is = ~0u;
        return ok;
    }

    bool init_port(u32 port_no) {
        sata_port = reinterpret_cast<volatile HBAPort*>(
            reinterpret_cast<volatile u8*>(hba) + 0x100 + port_no * 0x80
        );

        sata_port->cmd &= ~CMD_ST;
        port_wait_clear(sata_port, CMD_CR);

        u64 page_phys = pmm::alloc_frame();
        u64 page_virt = page_phys;
        vmm::map(page_virt, page_phys, vmm::KERNEL_RW);

        cmd_list_phys  = page_phys;
        fis_phys       = page_phys + 0x400;
        cmd_table_phys = page_phys + 0x500;
        identify_phys  = page_phys + 0x600;

        cmd_list_virt  = reinterpret_cast<CmdHeader*>(page_virt);
        fis_virt       = reinterpret_cast<u8*>(page_virt + 0x400);
        cmd_table_virt = reinterpret_cast<CmdTable*>(page_virt + 0x500);
        identify_data  = reinterpret_cast<u8*>(page_virt + 0x600);

        for (u16 i = 0; i < 4096; ++i)
            reinterpret_cast<u8*>(page_virt)[i] = 0;

        sata_port->clbl = cmd_list_phys & 0xFFFFFFFF;
        sata_port->clbh = 0;
        sata_port->fbl  = fis_phys & 0xFFFFFFFF;
        sata_port->fbh  = 0;
        sata_port->is   = ~0u;
        sata_port->serr = ~0u;

        sata_port->cmd |= CMD_FRE;
        wait_ms(1);
        sata_port->cmd |= CMD_ST;
        wait_ms(1);

        u32 sig = sata_port->sig;
        if ((sig >> 16) != 0xEB14 && (sig >> 16) != 0x9669) {
            (void)port_no;
            return false;
        }
        return true;
    }

    void send_identify() {
        setup_cmd_header(5, 1);  // CFL=5 (20-byte FIS = 5 DWORDs), 1 PRDT entry
        setup_cfis_and_prdt(ATA_IDENTIFY, 0, 0, identify_phys, 512);

        if (!issue_and_wait()) {
            serial::print("ahci: identify command failed\n");
            return;
        }

    }

}


namespace ahci {

    void init() {
        bool found = pci::enumerate([](const pci::Device& d) {
            if (d.class_code == 0x01 && d.subclass == 0x06 && d.prog_if == 0x01) {
                u64 abar = pci::get_bar(d.bus, d.device, d.function, 5);

                vmm::map(abar, abar, vmm::KERNEL_RW | vmm::PWT | vmm::PCD);
                hba = reinterpret_cast<volatile HBA*>(abar);

                hba->ghc |= GHC_AE;
                wait_ms(10);
                hba->ghc |= GHC_HR;
                wait_ms(100);
                hba->ghc &= ~GHC_HR;
                wait_ms(10);
                hba->ghc |= GHC_AE;

                u32 ports_impl = hba->pi;
                for (u8 i = 0; i < 32; ++i) {
                    if (ports_impl & (1u << i)) {
                        if (init_port(i)) {
                            send_identify();
                            serial::print("ahci: controller ready\n");
                        }
                        break;
                    }
                }
                return true;
            }
            return false;
        });

        if (!found) serial::print("ahci: no AHCI controller found\n");
    }

    bool read_sectors(u64 lba, u32 count, void* buffer) {
        if (!sata_port || !cmd_table_virt || count == 0) return false;

        u8* dst = reinterpret_cast<u8*>(buffer);

        // process up to 8 sectors (4KB) per DMA operation using a bounce
        // buffer allocated from the PMM. since the kernel is identity-mapped
        // at low addresses, phys == virt for PMM frames, so the HBA can DMA
        // directly to them and we then copy to the caller's buffer (which
        // may be heap-allocated at a non-identity-mapped virtual address).
        constexpr u32 MAX_SECTORS = 8;

        while (count > 0) {
            u32 n     = count > MAX_SECTORS ? MAX_SECTORS : count;
            u32 bytes = n * 512;

            u64 bounce = pmm::alloc_frame();

            setup_cmd_header(5, 1);   // CFL=5, W=0 (read), 1 PRDT entry
            setup_cfis_and_prdt(ATA_READ_DMA_EX, lba, n, bounce, bytes);

            bool ok = issue_and_wait();

            if (ok) {
                u8* src = reinterpret_cast<u8*>(bounce);
                for (u32 i = 0; i < bytes; ++i) dst[i] = src[i];
            }

            pmm::free_frame(bounce);

            if (!ok) {
                serial::print("ahci: read_sectors failed at LBA 0x");
                char buf[17]; hex::to_string(lba, buf); serial::print(buf);
                serial::print("\n");
                return false;
            }

            dst   += bytes;
            lba   += n;
            count -= n;
        }
        return true;
    }

}
