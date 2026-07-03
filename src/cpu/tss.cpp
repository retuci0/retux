#include "cpu/tss.hpp"

#include "lib/types.hpp"


// this must match the GDT layout in `boot.asm`:
//   0x00 = null, 0x08 = code, 0x10 = data, 0x18 = TSS (16-byte descriptor)
constexpr u16 TSS_SELECTOR = 0x18;

// every field offset is architecturally defined - do not reorder
struct TSS64 {
    u32 reserved0;
    u64 rsp[3];    // RSP0/1/2: stacks the CPU switches to on ring 0/1/2 entry
    u64 reserved1;
    u64 ist[7];    // IST1..IST7: dedicated stacks for specific exceptions
    u64 reserved2;
    u16 reserved3;
    u16 iopb;      // I/O permission bitmap offset (set past end to disable)
} __attribute__((packed));

// dedicated stack for double faults (#DF, vector 8).
// without this, a fault that corrupts RSP before firing (stack overflow, null
// write near the stack) would triple-fault instantly instead of giving you a
// register dump. with IST, the CPU unconditionally switches to this stack
// regardless of what RSP looked like at the time of the fault.
alignas(16) static u8 df_stack[8192];

// The TSS instance (statically allocated)
static TSS64 tss_;

// gdt64_tss_slot is the 16-byte placeholder in the GDT defined in boot.asm.
// we write the real TSS descriptor into it at runtime once we know the address of `tss_` above.
extern "C" u64 gdt64_tss_slot[2];

namespace {

    void write_tss_descriptor(u64 base, u32 limit) {
        // a 64-bit TSS descriptor is 16 bytes - a "system segment" extended to hold a full 64-bit base address.
        //
        // low qword layout:
        //   [15: 0]  limit[15:0]
        //   [39:16]  base[23:0]
        //   [47:40]  access byte: 0x89 = present=1, DPL=0, type=9 (available 64-bit TSS)
        //   [51:48]  limit[19:16]
        //   [55:52]  flags: 0 (G=0 means limit is in bytes, not 4KB pages)
        //   [63:56]  base[31:24]
        //
        // high qword layout:
        //   [31: 0]  base[63:32]
        //   [63:32]  reserved (must be zero)

        u64 low = 0;
        low |= static_cast<u64>(limit & 0xFFFF);
        low |= static_cast<u64>(base  & 0xFFFFFF)     << 16;
        low |= static_cast<u64>(0x89)                 << 40;
        low |= static_cast<u64>((limit >> 16) & 0xF)  << 48;
        low |= static_cast<u64>((base  >> 24) & 0xFF) << 56;

        u64 high = (base >> 32) & 0xFFFFFFFF;

        gdt64_tss_slot[0] = low;
        gdt64_tss_slot[1] = high;
    }

} // namespace

namespace tss {

    void init() {
        // IST entries hold the TOP of the stack (high address), not the base
        // ist[0] corresponds to IST1 in IDT gate descriptors (IST values
        // in the IDT are 1-indexed; 0 means "don't use IST").
        tss_.ist[0] = reinterpret_cast<u64>(df_stack + sizeof(df_stack));

        // setting iopb past the end of the TSS tells the CPU there is no
        // I/O permission bitmap - all port I/O from ring 3 will fault (which does
        // not exist yet but who cares)
        tss_.iopb = sizeof(TSS64);

        // write the descriptor into the GDT's TSS slot.
        // any exception routed to an IST slot will switch to the
        // corresponding stack before pushing its frame.
        write_tss_descriptor(
            reinterpret_cast<u64>(&tss_),
            sizeof(TSS64) - 1
        );

        // set up RSP0/1/2 with initial stack pointers. they'll be used for ring transition.
        tss_.rsp[0] = reinterpret_cast<uint64_t>(df_stack + sizeof(df_stack));  // RSP0 for kernel ring (0)
        tss_.rsp[1] = 0;  // RSP1 not used
        tss_.rsp[2] = 0;  // RSP2 not used

        asm volatile("ltr %%ax" : : "a"(TSS_SELECTOR));
    }

}
