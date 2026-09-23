#!/bin/sh
# usage: tests/run.sh [target...]
#
# Compiles each tests/*.ir, links, runs (via qemu for cross targets), and
# compares the exit code to the "# EXPECT N" directive.
#
# Modes (env CC_LINK_MODE):
#   raw   (default) — link with the target's ld; no libc; needs _start
#   libc            — link with the target's cc driver; pulls in glibc
#
# Tests tagged "# NEEDS_LIBC" run only in libc mode.
set -e
CC_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC="$CC_ROOT/cc-backend"
OUTDIR="$CC_ROOT/build/tests"
mkdir -p "$OUTDIR"

MODE="${CC_LINK_MODE:-raw}"

if [ $# -eq 0 ]; then targets="x86_64"; else targets="$*"; fi

pass=0; fail=0; skip=0
for ir in "$CC_ROOT"/tests/*.ir; do
    name=$(basename "$ir" .ir)
    expect=$(grep -m1 '^# EXPECT' "$ir" | awk '{print $3}')
    [ -z "$expect" ] && expect=0

    needs_libc=0
    grep -q '^# NEEDS_LIBC' "$ir" && needs_libc=1
    if [ "$needs_libc" = "1" ] && [ "$MODE" != "libc" ]; then
        skip=$((skip+1))
        continue
    fi

    for tgt in $targets; do
        case "$tgt" in
            arm64) RAW_LD=aarch64-linux-gnu-ld ; LIB_LD=aarch64-linux-gnu-gcc ; RUN=qemu-aarch64 ;;
            riscv) RAW_LD=riscv64-linux-gnu-ld ; LIB_LD=riscv64-linux-gnu-gcc ; RUN=qemu-riscv64 ;;
            arm)   RAW_LD=arm-linux-gnueabihf-ld ; LIB_LD=arm-linux-gnueabihf-gcc ; RUN=qemu-arm ;;
            x86)   RAW_LD="ld -m elf_i386" ; LIB_LD="gcc -m32" ; RUN="" ;;
            *)     RAW_LD=ld ; LIB_LD=gcc ; RUN="" ;;
        esac

        base="${name}_${tgt}"
        obj="$OUTDIR/$base.o"
        bin="$OUTDIR/$base.bin"

        if [ "$MODE" = "libc" ]; then
            CC_FLAGS="--libc"
            LINK="$LIB_LD -static"
        else
            CC_FLAGS=""
            LINK="$RAW_LD"
        fi

        if ! "$CC" $CC_FLAGS -o "$OUTDIR" "$tgt" "$ir" "$base.o" >/dev/null 2>&1; then
            echo "FAIL $name [$tgt]: compile failed"
            fail=$((fail+1)); continue
        fi
        if ! $LINK "$obj" -o "$bin" >/dev/null 2>&1; then
            echo "FAIL $name [$tgt]: link failed"
            fail=$((fail+1)); continue
        fi

        code=0
        $RUN "$bin" >/dev/null 2>&1 || code=$?

        if [ "$code" = "$expect" ]; then
            echo "PASS $name [$tgt]: exit=$code"
            pass=$((pass+1))
        else
            echo "FAIL $name [$tgt]: got $code want $expect"
            fail=$((fail+1))
        fi
    done
done
echo "---"
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" = "0" ]
