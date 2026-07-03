# contributing to retux

first off, thank you for considering contributing!  
this is a hobby OS, but clean, well‑documented code is always welcome.


## how to help
- **report bugs** - open an issue with a clear description and, if possible, the register dump from the serial log.
- **suggest features** - open an issue for discussion.
- **submit pull requests** - for bug fixes, improvements, or new features.


## code style
- **C++**: mostly C‑style with classes; avoid exceptions and RTTI.
  - use `u8`, `u16`, `u32`, `u64` from `types.hpp`.
  - keep functions short and well‑commented.
  - prefer `constexpr` over macros where possible.
- **assembly (NASM)**: Use `section .text`, `.data`, `.bss`. **indent with 4 spaces.**


## submitting a PR
1. fork the repo.
2. create a new branch.
3. make your changes.
4. run `make run` to ensure it still works.
5. write a descriptive commit message.
6. open a pull request against the `main` branch.


## need help?
feel free to reach out via the issue tracker, or email me at `erik@retucio.me`