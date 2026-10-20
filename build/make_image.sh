#!/bin/sh
# make_image.sh — загрузочный образ LispOS (M0/M1, BIOS/x86).
# Layout (LBA, 512B сектора):
#   LBA0        MBR (boot.bin)
#   LBA1..N     ядро (kernel.bin)              — flat binary, entry=0x90000
#   LBA(N+1)..  LibOS-Forth образ (libos/forth.img) — сырые байты FXE-образа
# Ядро читает сектор N+1 и далее через exokernel diskmap-право.
set -e
BUILD=${BUILD:-build}
IMG=${IMG:-$BUILD/lispos.img}
KERNEL_SECT_MAX=${KERNEL_SECT_MAX:-64}   # окно чтения MBR: 64 сектора = 32 KiB

test -f "$BUILD/boot.bin"   || { echo "missing $BUILD/boot.bin" >&2; exit 1; }
test -f "$BUILD/kernel.bin" || { echo "missing $BUILD/kernel.bin" >&2; exit 1; }

KSIZE=$(wc -c < "$BUILD/kernel.bin")
if [ "$KSIZE" -gt $((KERNEL_SECT_MAX * 512)) ]; then
    echo "kernel too big for loader window: $KSIZE > $((KERNEL_SECT_MAX*512))" >&2
    exit 1
fi
KSECT=$(( (KSIZE + 511) / 512 ))
[ "$KSECT" -lt 1 ] && KSECT=1

TMP="$IMG.tmp"

# --- слой 1: MBR + ядро, выровненное по секторам ---
{ cat "$BUILD/boot.bin"; cat "$BUILD/kernel.bin"; } > "$TMP"
PAD=$(( (512 - $(wc -c < "$TMP") % 512) % 512 ))
dd if=/dev/zero bs=1 count=$PAD >> "$TMP" 2>/dev/null

# --- слой 2: LibOS forth.img, начинающийся ровно после ядра ---
FSECT=0
if [ -f libos/forth.img ]; then
    FSIZE=$(wc -c < libos/forth.img)
    cat libos/forth.img >> "$TMP"
    PAD2=$(( (512 - $(wc -c < "$TMP") % 512) % 512 ))
    dd if=/dev/zero bs=1 count=$PAD2 >> "$TMP" 2>/dev/null
    FSECT=$(( (FSIZE + 511) / 512 ))
fi

mv "$TMP" "$IMG"
echo "image: $IMG ($(wc -c < "$IMG") bytes) kernel=$KSIZE B ($KSECT sect) forth_sect=$((1+KSECT)) forth=$FSECT sect"
