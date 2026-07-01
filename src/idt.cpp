#include "idt.hpp"
#include "hex.hpp"
#include "serial.hpp"
#include "types.hpp"
#include "vga.hpp"


// `isr_stub_table` lives in `isr.asm` - 32 entry-point addresses, one per CPU exception vector
extern "C" u64 isr_stub_table[32];

namespace {

    struct GateDescriptor {
        u16 offset_low;
        u16 selector;
        u8  ist;        // low 3 bits: IST index (0 = use current stack)
        u8  type_attr;  // present, DPL, gate type
        u16 offset_mid;
        u32 offset_high;
        u32 reserved;
    } __attribute__((packed));

    struct IDTPointer {
        u16 limit;
        u64 base;
    } __attribute__((packed));

    constexpr int IDT_ENTRIES = 256;
    constexpr u8 INTERRUPT_GATE = 0x8E;         // present=1, DPL=0, type=0xE
    constexpr u16 KERNEL_CODE_SELECTOR = 0x08;  // .code_segment in boot.asm's GDT

    GateDescriptor idt_table[IDT_ENTRIES];
    IDTPointer idt_ptr;

    void set_gate(int vector, u64 handler, u8 ist = 0) {
        idt_table[vector].offset_low  = handler & 0xFFFF;
        idt_table[vector].selector    = KERNEL_CODE_SELECTOR;
        idt_table[vector].ist         = ist;
        idt_table[vector].type_attr   = INTERRUPT_GATE;
        idt_table[vector].offset_mid  = (handler >> 16) & 0xFFFF;
        idt_table[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
        idt_table[vector].reserved    = 0;
    }

} // namespace

namespace idt {

    void init() {
        for (int i = 0; i < 32; ++i) {
            set_gate(i, isr_stub_table[i]);
        }

        // #DF (vector 8) gets IST1 - the dedicated double-fault stack set up
        // by tss::init(). re-calling set_gate for vector 8 overwrites the entry
        // from the loop above with the same handler but ist=1 instead of 0.
        set_gate(8, isr_stub_table[8], 1);

        // vectors 32-255 stay zeroed since firing one of those now would hit
        // a non-present IDT entry and raise #GP

        idt_ptr.limit = sizeof(idt_table) - 1;
        idt_ptr.base  = reinterpret_cast<u64>(&idt_table);
        asm volatile("lidt %0" : : "m"(idt_ptr));

        // deliberately not calling `sti` here
    }

}

// mirrors `isr.asm`'s push order exactly, low address to high address
struct InterruptFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
} __attribute__((packed));


namespace {

    constexpr const char* exception_names[32] = {
        "divide error", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range exceeded", "invalid opcode", "device not available",
        "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
        "stack-segment fault", "general protection fault", "page fault", "reserved",
        "x87 floating-point exception", "alignment check", "machine check", "SIMD floating-point exception",
        "virtualization exception", "control protection exception", "reserved", "reserved",
        "reserved", "reserved", "reserved", "reserved",
        "reserved", "reserved", "reserved", "reserved"
    };

    void print_field(const char* label, u64 value) {
        char buf[17];
        hex::to_string(value, buf);
        serial::print(label);
        serial::print(buf);
        serial::print("\n");
    }

}


extern "C" void isr_common_handler(InterruptFrame* frame) {
    serial::print("\n--- unhandled exception ---\n");
    serial::print("vector: ");
    {
        char buf[17];
        hex::to_string(frame->vector, buf);
        serial::print(buf);
    }
    serial::print("  (");
    serial::print(exception_names[frame->vector < 32 ? frame->vector : 31]);
    serial::print(")\n");

    print_field("error code: ", frame->error_code);
    print_field("rip:        ", frame->rip);
    print_field("cs:         ", frame->cs);
    print_field("rflags:     ", frame->rflags);
    print_field("rsp:        ", frame->rsp);
    print_field("ss:         ", frame->ss);
    print_field("rax:        ", frame->rax);
    print_field("rbx:        ", frame->rbx);
    print_field("rcx:        ", frame->rcx);
    print_field("rdx:        ", frame->rdx);
    print_field("rsi:        ", frame->rsi);
    print_field("rdi:        ", frame->rdi);
    print_field("rbp:        ", frame->rbp);

    vga::print("\nKERNEL PANIC: unhandled exception (see serial output)\n");

    // no recovery path exists yet. just disable interrupts in case it's a nested fault and stop.
    asm volatile("cli");
    while (true) {
        asm volatile("hlt");
    }
}