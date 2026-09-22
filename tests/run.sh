#!/bin/sh
# usage: tests/run.sh [target...]
# For each tests/*.ir, read "# EXPECT N", compile for each target,
# link + run, compare exit code. Objects go into build/tests/.
set -e
CC_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC="$CC_ROOT/cc-backend"
OUTDIR="$CC_ROOT/build/tests"
mkdir -p "$OUTDIR"

if [ $# -eq 0 ]; then targets="x86_64"; else targets="$*"; fi

pass=0; fail=0
for ir in "$CC_ROOT"/tests/*.ir; do
    name=$(basename "$ir" .ir)
    expect=$(grep -m1 '^# EXPECT' "$ir" | awk '{print $3}')
    [ -z "$expect" ] && expect=0
    for tgt in $targets; do
        case "$tgt" in
            arm64) CC_LD=aarch64-linux-gnu-gcc ; RUN=qemu-aarch64 ;;
            arm)   CC_LD=arm-linux-gnueabihf-gcc ; RUN=qemu-arm ;;
            riscv) CC_LD=riscv64-linux-gnu-gcc ; RUN=qemu-riscv64 ;;
            x86)   CC_LD="gcc -m32" ; RUN="" ;;
            *)     CC_LD=gcc ; RUN="" ;;
        esac
        base="${name}_${tgt}"
        obj="$OUTDIR/$base.o"
        bin="$OUTDIR/$base.bin"
        if ! "$CC" -o "$OUTDIR" "$tgt" "$ir" "$base.o" >/dev/null 2>&1; then
            echo "FAIL $name [$tgt]: compile failed"
            fail=$((fail+1)); continue
        fi
        if ! $CC_LD -static "$obj" -o "$bin" >/dev/null 2>&1; then
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
echo "$pass passed, $fail failed"
[ "$fail" = "0" ]
