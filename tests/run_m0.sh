#!/bin/sh
# run_m0.sh — автотест этапа M0: QEMU грузит образ, проверяем маркеры в serial-выводе.
set -e
BUILD=${BUILD:-build}
IMG=$BUILD/lispos.img
OUT=$BUILD/m0_serial.log
TIMEOUT=${TIMEOUT:-30}

test -f "$IMG" || { echo "image missing: $IMG"; exit 1; }

rm -f "$OUT"
# -display none: только serial; -no-reboot: не зацикливать fault; таймер убьёт qemu
( timeout -s INT "$TIMEOUT" qemu-system-i386 \
    -drive format=raw,file="$IMG" \
    -serial stdio -display none -no-reboot > "$OUT" 2>&1 ) || true

fail() { echo "M0 TEST FAIL: $1"; echo "--- serial log ---"; cat "$OUT"; exit 1; }

grep -q "LispOS MBR loading" "$OUT" || fail "MBR banner not found (загрузчик не отработал)"
grep -q "M0 OK" "$OUT" || fail "kernel banner not found (PM переход/прыжок в ядро не случился)"
grep -q "boot magic     : 4B50534C" "$OUT" || fail "BootInfo magic mismatch"
grep -q "kernel load pa : 00090000" "$OUT" || fail "kernel load PA mismatch (ожидался 0x90000)"
grep -q "entering idle loop" "$OUT" || fail "kernel did not reach idle"

echo "M0 TEST PASS"
