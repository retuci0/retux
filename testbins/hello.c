int main(void) {
    const char msg[] = "hello from a real Linux ELF binary!\n";
    __asm__ volatile(
        "syscall"
        :
        : "a"(1), "D"(2), "S"(msg), "d"(sizeof(msg) - 1)
        : "rcx", "r11", "memory"
    );
    __asm__ volatile("syscall" : : "a"(60), "D"(0) : "memory");
    __builtin_unreachable();
}
