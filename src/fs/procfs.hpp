#pragma once

#include "fs/vfs.hpp"


// minimal procfs: three flat files (/proc/uptime, /proc/meminfo,
// /proc/cpuinfo), content generated fresh at open(). enough for fastfetch to
// read plausible system info instead of -ENOENT. no general procfs, no /sys.

namespace procfs {

    // mount at "/proc" via vfs::mount_at("/proc", procfs::mount()) - see main.cpp.
    vfs::SuperBlock* mount();

}
