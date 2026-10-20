/* bootinfo.h — exokernel ABI: структура BootInfo (этап M0, BIOS/x86)
 * Передаётся ядру регистрами после MBR (см. boot/boot.asm):
 *   EAX = 'LSPK' magic, EBX = физ. адрес загрузки ядра, ECX = число дисков, EDX = boot drive.
 * Ядро копирует их в BootInfo и хранит для LibOS (экзядро раздаёт права на ресурсы).
 */
#ifndef KERNEL_BOOTINFO_H
#define KERNEL_BOOTINFO_H

#include <stdint.h>

#define BOOT_MAGIC 0x4B50534Cu /* 'LSPK' little-endian */

typedef struct {
    uint32_t magic;          /* BOOT_MAGIC */
    uint32_t kernel_load_pa; /* физ. адрес, куда MBR загрузил ядро */
    uint32_t kernel_size;    /* размер ядра в байтах (заполняет ядро сам) */
    uint16_t boot_drive;     /* INT13h drive number */
    uint16_t drives_count;   /* число найденных дисков (M0: 1) */
    uint32_t mem_mb;         /* объём RAM из e820/e801 (M0: заглушка) */
} BootInfo;

#endif /* KERNEL_BOOTINFO_H */
