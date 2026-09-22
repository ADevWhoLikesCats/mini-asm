# cc-backend

A multi-target compiler backend. Reads a small SSA IR, emits assembly,
assembles with the cross toolchain, produces object files.

## Targets

| target | asm    | obj                | C driver               | emulator       |
|--------|--------|--------------------|------------------------|----------------|
| x86_64 | AT&T   | ELF64 x86_64       | `gcc`                  | native         |
| arm64  | AArch64| ELF64 aarch64      | `aarch64-linux-gnu-gcc`| `qemu-aarch64` |
| riscv  | RV64   | ELF64 riscv64      | `riscv64-linux-gnu-gcc`| `qemu-riscv64` |
| x86    | AT&T   | ELF32 i386         | `gcc -m32`             | native         |
| arm    | ARMv7  | ELF32 arm          | `arm-linux-gnueabihf-gcc` | `qemu-arm` |

## Dependencies (Ubuntu)

    sudo apt install -y \
        binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu \
        binutils-riscv64-linux-gnu gcc-riscv64-linux-gnu \
        binutils-arm-linux-gnueabihf gcc-arm-linux-gnueabihf \
        gcc-multilib qemu-user

## Build

    make

## Usage

    ./cc-backend <target> <input.ir> <output.o>

Example:

    ./cc-backend x86_64 tests/t1_add.ir build/t1.o
    gcc -static build/t1.o -o build/t1 && ./build/t1

## Test

    make test                # default: x86_64
    TARGETS="x86_64 arm64 riscv x86" make test

Or directly:

    ./tests/run.sh x86_64 arm64 riscv x86

Test IR files live in `tests/*.ir`. Each file declares its expected exit
code with a `# EXPECT N` first line.

## Writing IR

See `tests/*.ir` for examples. The builder API in `include/ir_builder.h`
lets you construct IR programmatically from C.
