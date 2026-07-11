# testbins

static, non-PIE musl test binaries for exercising retux's Linux-ABI syscall
surface (`cpu/syscall.cpp`, `task/elf.cpp`, `task/user.cpp`). not part of the
kernel build - a dev-loop tool for bring-up against real ELF binaries, built
via a throwaway Alpine (musl libc) Docker container so the host doesn't need
its own musl cross-compiler:

```sh
docker run --rm -v "$PWD":/src -w /src alpine sh -c \
  'apk add --no-cache gcc musl-dev && gcc -static -no-pie -o hello hello.c'
```

`-static -no-pie` matters: `task/elf.cpp` only loads `ET_EXEC` (no dynamic
linker, no relocations), and retux's kernel supplies no `/lib64/ld-linux*`.

to try one under retux: copy the built binary into `initrdroot/`, point
`task::user::spawn_from_elf("/whatever")` at it in `main.cpp` (only one
resident ring-3 task at a time right now - see `task.hpp`), then
`make run`. an unimplemented syscall prints its number to serial instead of
silently misbehaving.
