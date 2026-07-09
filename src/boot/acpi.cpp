#include "boot/acpi.hpp"
#include "boot/mb2.hpp"

#include "lib/types.hpp"
#include "io/serial.hpp"


namespace {

    // ACPI 1.0 RSDP - what the Multiboot2 "ACPI old" tag copies verbatim.
    struct RSDPDescriptor {
        char signature[8];  // "RSD PTR "
        u8   checksum;
        char oem_id[6];
        u8   revision;
        u32  rsdt_address;
    } __attribute__((packed));

    // ACPI >=2.0 extends the above with a 64-bit XSDT pointer. this is what
    // the Multiboot2 "ACPI new" tag copies.
    struct RSDPDescriptor20 {
        RSDPDescriptor v1;
        u32 length;
        u64 xsdt_address;
        u8  extended_checksum;
        u8  reserved[3];
    } __attribute__((packed));

    // every ACPI table (RSDT, XSDT, MADT, ...) starts with this.
    struct SDTHeader {
        char signature[4];
        u32  length;
        u8   revision;
        u8   checksum;
        char oem_id[6];
        char oem_table_id[8];
        u32  oem_revision;
        u32  creator_id;
        u32  creator_revision;
    } __attribute__((packed));

    // MADT ("APIC" table) body, right after the SDTHeader.
    struct MADT {
        SDTHeader header;
        u32 local_apic_addr;
        u32 flags;
        // followed by a variable-length stream of MADTEntryHeader-prefixed records
    } __attribute__((packed));

    struct MADTEntryHeader {
        u8 type;
        u8 length;
    } __attribute__((packed));

    constexpr u8 MADT_TYPE_IOAPIC   = 1;
    constexpr u8 MADT_TYPE_OVERRIDE = 2;

    struct MADTIoApic {
        MADTEntryHeader header;
        u8  io_apic_id;
        u8  reserved;
        u32 io_apic_addr;
        u32 gsi_base;
    } __attribute__((packed));

    struct MADTIntOverride {
        MADTEntryHeader header;
        u8  bus_source;
        u8  irq_source;
        u32 gsi;
        u16 flags;
    } __attribute__((packed));

    bool signature_is(const SDTHeader* hdr, const char* sig) {
        return hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1]
            && hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3];
    }

    // find any ACPI table by its 4-character signature ("APIC", "HPET", ...)
    // by walking either the RSDT (32-bit pointers) or the XSDT (64-bit
    // pointers) - same search, different pointer width.
    const SDTHeader* find_table_rsdt(u64 rsdt_addr, const char* sig) {
        const auto* rsdt = reinterpret_cast<const SDTHeader*>(rsdt_addr);
        u32 entries = (rsdt->length - sizeof(SDTHeader)) / 4;
        const auto* ptrs = reinterpret_cast<const u32*>(rsdt_addr + sizeof(SDTHeader));
        for (u32 i = 0; i < entries; ++i) {
            const auto* hdr = reinterpret_cast<const SDTHeader*>(static_cast<u64>(ptrs[i]));
            if (signature_is(hdr, sig)) return hdr;
        }
        return nullptr;
    }

    const SDTHeader* find_table_xsdt(u64 xsdt_addr, const char* sig) {
        const auto* xsdt = reinterpret_cast<const SDTHeader*>(xsdt_addr);
        u32 entries = (xsdt->length - sizeof(SDTHeader)) / 8;
        const auto* ptrs = reinterpret_cast<const u64*>(xsdt_addr + sizeof(SDTHeader));
        for (u32 i = 0; i < entries; ++i) {
            const auto* hdr = reinterpret_cast<const SDTHeader*>(ptrs[i]);
            if (signature_is(hdr, sig)) return hdr;
        }
        return nullptr;
    }

    // HPET Description Table (signature "HPET"). the address field is a
    // 12-byte ACPI Generic Address Structure; we only support the common
    // case (address_space_id == 0, meaning "system memory" / MMIO).
    struct HPETTable {
        SDTHeader header;
        u32 event_timer_block_id;
        u8  gas_address_space_id;
        u8  gas_register_bit_width;
        u8  gas_register_bit_offset;
        u8  gas_reserved;
        u64 gas_address;
        u8  hpet_number;
        u16 minimum_tick;
        u8  page_protection;
    } __attribute__((packed));

}

namespace acpi {

    namespace {
        u64    lapic_addr_    = 0;
        IoApic ioapics_[MAX_IOAPICS];
        int    ioapic_count_  = 0;
        u32    irq_to_gsi_[16];
        HpetInfo hpet_;
    }

    u64 lapic_address()          { return lapic_addr_; }
    int ioapic_count()           { return ioapic_count_; }
    const IoApic& ioapic(int i)  { return ioapics_[i]; }
    const HpetInfo& hpet()       { return hpet_; }

    u32 irq_to_gsi(u8 irq) {
        return irq < 16 ? irq_to_gsi_[irq] : irq;
    }

    bool init(u64 boot_info_addr) {
        // identity mapping until an override says otherwise
        for (int i = 0; i < 16; ++i) irq_to_gsi_[i] = i;

        const auto* old_tag = mb2::find_tag(boot_info_addr, mb2::TAG_ACPI_OLD);
        const auto* new_tag = mb2::find_tag(boot_info_addr, mb2::TAG_ACPI_NEW);
        if (!old_tag && !new_tag) {
            serial::print("acpi: bootloader gave us no RSDP tag\n");
            return false;
        }

        // prefer the ACPI 2.0+ RSDP (has the XSDT) when present, since the
        // XSDT is a strict superset of what the RSDT can tell us.
        u64  root_addr = 0;
        bool root_is_xsdt = false;

        if (new_tag) {
            const auto* rsdp = reinterpret_cast<const RSDPDescriptor20*>(
                reinterpret_cast<const u8*>(new_tag) + sizeof(mb2::Tag));
            if (rsdp->xsdt_address) {
                root_addr = rsdp->xsdt_address;
                root_is_xsdt = true;
            }
        }
        if (!root_addr && old_tag) {
            const auto* rsdp = reinterpret_cast<const RSDPDescriptor*>(
                reinterpret_cast<const u8*>(old_tag) + sizeof(mb2::Tag));
            root_addr = rsdp->rsdt_address;
        }

        if (!root_addr) {
            serial::print("acpi: RSDP present but no usable RSDT/XSDT pointer\n");
            return false;
        }

        auto find = [&](const char* sig) {
            return root_is_xsdt ? find_table_xsdt(root_addr, sig)
                                 : find_table_rsdt(root_addr, sig);
        };

        const auto* madt = reinterpret_cast<const MADT*>(find("APIC"));
        if (!madt) {
            serial::print("acpi: no MADT ('APIC' table) found\n");
            return false;
        }

        if (const auto* hpet_table = reinterpret_cast<const HPETTable*>(find("HPET"))) {
            // address_space_id == 0 means "system memory" (plain MMIO),
            // which is what every real HPET implementation actually uses.
            if (hpet_table->gas_address_space_id == 0) {
                hpet_.present = true;
                hpet_.address = hpet_table->gas_address;
            }
        }

        lapic_addr_ = madt->local_apic_addr;

        const u8* p   = reinterpret_cast<const u8*>(madt) + sizeof(MADT);
        const u8* end = reinterpret_cast<const u8*>(madt) + madt->header.length;

        while (p < end) {
            const auto* entry = reinterpret_cast<const MADTEntryHeader*>(p);
            if (entry->length == 0) break;  // malformed - bail rather than loop forever

            if (entry->type == MADT_TYPE_IOAPIC && ioapic_count_ < MAX_IOAPICS) {
                const auto* io = reinterpret_cast<const MADTIoApic*>(p);
                ioapics_[ioapic_count_].address  = io->io_apic_addr;
                ioapics_[ioapic_count_].gsi_base = io->gsi_base;
                ++ioapic_count_;
            } else if (entry->type == MADT_TYPE_OVERRIDE) {
                const auto* ov = reinterpret_cast<const MADTIntOverride*>(p);
                if (ov->irq_source < 16) irq_to_gsi_[ov->irq_source] = ov->gsi;
            }

            p += entry->length;
        }

        serial::print(hpet_.present ? "acpi: APIC and HPET found\n"
                                         : "acpi: APIC found\n");
        return ioapic_count_ > 0;
    }

}
