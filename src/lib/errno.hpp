#pragma once

#include "lib/types.hpp"


// Linux errno values that retux's syscall implementations actually return.
// the syscall ABI returns `-errno` directly in rax - no separate errno
// variable at the kernel/user boundary (userspace libc splits that out).

namespace err {

    constexpr i64 ENOENT = 2;
    constexpr i64 EIO    = 5;
    constexpr i64 EBADF  = 9;
    constexpr i64 ENOMEM = 12;
    constexpr i64 EINVAL = 22;
    constexpr i64 ENOTTY = 25;
    constexpr i64 ENODEV = 19;
    constexpr i64 EMFILE = 24;
    constexpr i64 ENOSYS = 38;

}
