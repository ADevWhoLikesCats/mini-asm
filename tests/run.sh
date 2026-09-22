#!/bin/sh
# usage: tests/run.sh [target...]
# Compiles each tests/*.ir, links with the target's linker, runs (via qemu if
# cross), and compares the exit code to the # EXPECT N directive.
# Tests tagged "# NEEDS_LIBC" are skipped unless CC_LINK_MODE=libc.
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
    if grep -q '^# NEEDS_LIBC' "$ir" && [ "$MODE" != "libc" ]; then
        skip=$((skip+1))
        continue
    fi
    for tgt in $targets; do
        case "$tgt" in
            arm64) LD=aarch64-linux-gnu-ld ; RUN=qemu-aarch64 ;;
            riscv) LD=riscv64-linux-gnu-ld ; RUN=qemu-riscv64 ;;
            arm)   LD=arm-linux-gnueabihf-ld ; RUN=qemu-arm ;;
            x86)   LD="ld -m elf_i386" ; RUN="" ;;
            *)     LD=ld ; RUN="" ;;
        esac
        base="${name}_${tgt}"
        obj="$OUTDIR/$base.o"
        bin="$OUTDIR/$base.bin"
        if ! "$CC" -o "$OUTDIR" "$tgt" "$ir" "$base.o" >/dev/null 2>&1; then
            echo "FAIL $name [$tgt]: compile failed"
            fail=$((fail+1)); continue
        fi
        if ! $LD "$obj" -o "$bin" >/dev/null 2>&1; then
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
