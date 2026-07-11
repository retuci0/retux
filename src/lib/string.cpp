#include "lib/string.hpp"


namespace string {

    void* memcpy(void* dest, const void* src, size_t n) {
        u8* d = (u8*)dest;
        const u8* s = (const u8*)src;
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
        return dest;
    }

    void* memset(void* dest, int c, size_t n) {
        u8* d = (u8*)dest;
        for (size_t i = 0; i < n; ++i) d[i] = (u8)c;
        return dest;
    }

    int strcmp(const char* s1, const char* s2) {
        while (*s1 && *s2 && *s1 == *s2) { ++s1; ++s2; }
        return (int)(u8)*s1 - (int)(u8)*s2;
    }

    int strncmp(const char* s1, const char* s2, size_t n) {
        while (n && *s1 && *s1 == *s2) { ++s1; ++s2; --n; }
        if (n == 0) return 0;
        return (int)(u8)*s1 - (int)(u8)*s2;
    }

    size_t strlen(const char* s) {
        size_t n = 0;
        while (*s++) ++n;
        return n;
    }

    size_t strnlen(const char* s, size_t max) {
        size_t n = 0;
        while (n < max && *s++) ++n;
        return n;
    }

    char* strcpy(char* dest, const char* src) {
        char* d = dest;
        while ((*d++ = *src++));
        return dest;
    }

    char* strncpy(char* dest, const char* src, size_t n) {
        char* d = dest;
        while (n-- && (*d++ = *src++));
        while (n--) *d++ = '\0';
        return dest;
    }

}
