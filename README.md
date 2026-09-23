# mini-asm

A multi-target compiler backend meant to be used for hobby compilers.

Reads a small SSA IR, optimizes it, emits assembly for 5 architectures,
assembles with the cross toolchain, and produces runnable binaries. Includes
a text-IR parser, a C API for building IR programmatically, an optimizing
pass pipeline, and a linear-scan register allocator.

## Targets

| target | asm     | object          | assembler                | linker (raw)            | runner          |
|--------|---------|-----------------|--------------------------|-------------------------|-----------------|
| x86_64 | AT&T    | ELF64 x86_64    | `as --64`                | `ld`                    | native          |
| arm64  | AArch64 | ELF64 aarch64   | `aarch64-linux-gnu-as`   | `aarch64-linux-gnu-ld`  | `qemu-aarch64`  |
| riscv  | RV64    | ELF64 riscv64   | `riscv64-linux-gnu-as`   | `riscv64-linux-gnu-ld`  | `qemu-riscv64`  |
| x86    | AT&T    | ELF32 i386      | `as --32`                | `ld -m elf_i386`        | native          |
| arm    | ARMv7   | ELF32 arm       | `arm-linux-gnueabihf-as` | `arm-linux-gnueabihf-ld`| `qemu-arm`      |

## Features

**IR**
- SSA-form IR with typed values (`i1/i8/i16/i32/i64/f32/f64/ptr`),
  basic blocks, and 2-predecessor phi nodes
- Text parser for `.ir` files
- Programmatic builder API (`include/ir_builder.h`), including a
  placeholder mechanism that supports phi cycles and forward references
- Named types (structs, arrays), string literals in `.rodata`

**Opcodes**
- Integer: `add sub mul sdiv udiv srem urem and or xor shl lshr ashr neg not`
- Float: `fadd fsub fmul fdiv fneg fcmp`
- Comparisons: all 10 `icmp` predicates
- Conversions: `zext sext trunc sitofp uitofp fptosi fptoui fpext fptrunc`
- Memory: `alloca load store gep gep_field`
- Control flow: `br cbr ret unreachable phi`
- Calls: `call` with up to 16 args (registers + stack per target ABI)

**Optimizer**
- Constant folding
- Algebraic simplification (`x+0`, `x*1`, `x*0`, `x&0`, `x|0`, `x^0`, `x<<0`, ...)
- Copy propagation
- Common subexpression elimination (intra-block)
- Dead code elimination
- Dead block elimination and constant-`cbr` folding
- All passes run to fixpoint

**Codegen**
- Linear-scan register allocation with liveness analysis, call-clobbering
  handling, and loop live-range extension
- Per-target register pools
- Phi copies emitted on the correct edges via edge-split stubs
- SysV (x86_64), AAPCS64 (arm64), SysV-RV64, cdecl (x86), AAPCS (arm32)
- AArch64/ARMv7 FP via VFP/NEON registers; x86/x86_64 via SSE2
- riscv64 uses `ld`/`sd` with large-offset fallbacks

**Linking**
- **Raw mode** (default): links with `ld` and no libc. The backend emits a
  `_start` symbol that calls `main` and exits via a direct syscall.
  Objects are tiny and standalone. Works on all 5 targets.
- **Libc mode** (`--libc`): links with `gcc` / `aarch64-linux-gnu-gcc` /
  etc., pulling in glibc. Enables `printf`, `malloc`, and friends.
  Works on x86_64, arm64, riscv, arm32.

## Status

All targets pass the full test suite in raw mode (65/65) and the
libc-enabled subset in libc mode (64/64 across 4 targets).

| Test suite         | x86_64 | arm64 | riscv | x86 | arm |
|--------------------|:------:|:-----:|:-----:|:---:|:---:|
| raw (12 tests)     |   ✅   |   ✅  |   ✅  |  ✅ |  ✅ |
| libc (14 tests)    |   ✅   |   ✅  |   ✅  |  —  |  ✅ |

(x86 32-bit is raw-mode only; `gcc-multilib` conflicts with the cross-gcc
packages on Debian/Ubuntu.)

## Dependencies (Ubuntu)

For **raw mode** — the default:

    sudo apt install -y \
        binutils-aarch64-linux-gnu \
        binutils-riscv64-linux-gnu \
        binutils-arm-linux-gnueabihf \
        qemu-user

For **libc mode** — add the cross C compilers:

    sudo apt install -y \
        gcc-aarch64-linux-gnu \
        gcc-riscv64-linux-gnu \
        gcc-arm-linux-gnueabihf

**Note:** `gcc-multilib` (needed for `gcc -m32` on the host) conflicts with
the cross-gcc packages. Pick one. Raw mode for x86 32-bit works either way.

## Build

    make

## Usage

    ./cc-backend [--libc] [-O0|-O1] [-o <outdir>] <target> <input.ir> <output.o>

Flags:
- `--libc`       — link with libc via the target's C compiler driver
- `-O0`          — disable optimizer passes
- `-O1`          — enable constfold, algebraic, CSE, copy prop, DCE (default)
- `-o <dir>`     — output directory for `.s` and `.o` (default: `build`)

Environment:
- `CC_BACKEND_OUT=<dir>` — same as `-o`
- `CC_DUMP_IR=1`         — print parsed IR to stderr

## Example

Given `tests/t1_add.ir`:

    func main i32 {
    block entry
      %0 = add i32 10 20
      %1 = mul i32 %0 3
      ret i32 %1
    }

Compile, assemble, link, and run on x86_64:

    ./cc-backend x86_64 tests/t1_add.ir build/t1.o
    ld build/t1.o -o build/t1
    ./build/t1; echo $?     # 90

The optimizer folds the entire expression at compile time. The emitted
`main` is:

    main:
      pushq %rbp
      movq %rsp, %rbp
      movq $90, %rax
      leave
      ret

Cross-compile and run under qemu:

    ./cc-backend arm64 tests/t1_add.ir build/t1_arm64.o
    aarch64-linux-gnu-ld build/t1_arm64.o -o build/t1_arm64
    qemu-aarch64 build/t1_arm64; echo $?   # 90

With libc, real programs run:

    ./cc-backend --libc x86_64 tests/t13_hello_world.ir build/hello.o
    gcc -static build/hello.o -o build/hello
    ./build/hello           # prints "Hello, world!"

## Test

    make test                                    # x86_64 only
    ./tests/run.sh x86_64 arm64 riscv x86 arm    # all 5, raw mode
    CC_LINK_MODE=libc ./tests/run.sh x86_64 arm64 riscv arm

Tests live in `tests/*.ir`. Each file declares its expected exit code with
a `# EXPECT N` first line. Tests that require libc are tagged
`# NEEDS_LIBC` and skipped in raw mode.

## Writing IR

**Text form** — see `tests/*.ir`. Syntax:

    func name ret (arg_types) {
    block entry
      %0 = add i32 10 20
      %1 = icmp i32 slt %0 100
      cbr %1 loop done
    block loop
      ...
    block done
      ret i32 %0
    }

Supported: `add sub mul sdiv udiv srem urem and or xor shl lshr ashr neg not`
· `fadd fsub fmul fdiv fneg fcmp` · `load store alloca gep gep_field` ·
`br cbr ret phi call` · `str "string literal"` · `zext sext trunc
sitofp uitofp fptosi fptoui fpext fptrunc` · `icmp` with `eq ne slt sle
sgt sge ult ule ugt uge` · `fcmp` with `oeq one olt ole ogt oge ord uno` ·
`type Name = { T1, T2 }` · `type Name = T[N]`.

**Builder API** — `include/ir_builder.h`. Example:

    #include "ir_builder.h"

    IRModule  *m   = ir_module_new();
    IRBuilder *b   = ir_builder_new(m);
    IRType    *i32 = ir_type_i32();

    IRFunc  *f = ir_builder_func(b, "main", i32);
    IRBlock *e = ir_builder_block(b, f, "entry");
    ir_set_insert(b, e);

    IRValue *sum  = ir_add(b, i32, ir_const_i(i32, 10), ir_const_i(i32, 20));
    IRValue *prod = ir_mul(b, i32, sum, ir_const_i(i32, 3));
    ir_ret(b, prod);

Phi cycles use the placeholder API:

    IRValue *i_ph = ir_value_placeholder(b, i32);
    IRValue *next = ir_add(b, i32, i_ph, ir_const_i(i32, 1));
    IRValue *i    = ir_phi(b, i32, zero, entry, next, loop);
    ir_value_define(b, i_ph, i);
    ir_builder_finalize(b);

## Project layout

    include/
      ir.h              IR types, opcodes, IRInstr/IRBlock/IRFunc/IRModule
      ir_builder.h      programmatic IR construction
      ir_parse.h        text IR parser
      opt.h             optimizer passes
      backend.h         target interface, regalloc, TargetDesc
      targets/*.h       per-target headers
    src/
      ir/
        ir.c            IR construction and allocation
        ir_parse.c      text parser
        ir_print.c      IR dump for debugging
        ir_fpimm.c      shared FP immediate table
        ir_builder.c    builder implementation
      backend/
        backend.c       target dispatch
        regalloc.c      linear-scan register allocator
        opt.c           constant folding, CSE, DCE, etc.
      targets/
        x86_64/ arm64/ riscv/ x86/ arm/
          isel.c        instruction selection and emission
          abi.c         TargetDesc
    tools/
      cc-backend.c      command-line driver
      bt_*.c            builder-driven test programs
    tests/
      *.ir              test inputs
      run.sh            test runner

## Comparison

The emitted code quality is comparable to TCC or chibicc at -O0 for
straight-line code, and better for code dominated by constants (which
gets folded at IR level). It is not competitive with LLVM or GCC — those
have global value numbering, loop transforms, inlining, alias analysis,
and hundreds of other passes.

The optimizer is intentionally small: the goal is a clean, understandable
backend, not a world-class optimizing compiler.

## License

Apache 2.0. See `LICENSE`.

## Embedding

`make lib` produces `libmini-asm.a`, containing the IR, parser, builder,
optimizer, regallocator, and all five backends. A downstream compiler can
link against it:

    #include "ir.h"
    #include "ir_builder.h"
    #include "backend.h"

    IRModule  *m = ir_module_new();
    IRBuilder *b = ir_builder_new(m);
    /* ... build IR ... */
    const TargetDesc *t = backend_lookup("x86_64");
    FILE *f = fopen("out.s", "w");
    target_emit_dispatch(m, f, t);
    fclose(f);

Compile with:

    gcc -I/path/to/mini-asm/include yourcode.c -L/path/to/mini-asm -lmini-asm

Or install system-wide:

    sudo make install             # to /usr/local
    sudo make install PREFIX=/usr # to /usr
