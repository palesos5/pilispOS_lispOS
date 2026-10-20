/* disk.c — protected resource "disk" (модель Aegis/ExOS, agent.md §4.8).
 *
 * LibOS не имеет прямого доступа к INT 13h: ядро проверяет каждый запрос по
 * таблице прав diskmap и выполняет IO через 16-битный trampoline.
 *
 * Механизм (без VM8086): GDT загрузчика дополнена 16-битным code сегментом
 * (селектор 0x28). Из PM32 выполняем `lcall $0x28,$STUB_PA` — CS переключается
 * в 16-битный режим; stub с DS=ES=SS=0 работает с фиксированными физ адресами
 * управления, вызывает INT 13h AH=42h/43h (DAP), затем `ljmp $0x08,$resume`,
 * где resume — метка сразу после lcall (адрес патчится rm_call до вызова).
 * ESP не меняется => 32-битный стек остаётся валидным. IF=0 (cli) всё время.
 */
#include <stdint.h>
#include "exo_abi.h"

DiskMapEntry g_diskmap[DISKMAP_MAX];
int          g_diskmap_count;

static uint8_t g_drive = 0x80;

void disk_set_drive(uint8_t d) { g_drive = d; }

int diskmap_check(uint32_t lba, uint32_t nsect, int write)
{
    for (int i = 0; i < DISKMAP_MAX; i++) {
        const DiskMapEntry *e = &g_diskmap[i];
        if (!e->used) continue;
        if (lba >= e->lba_start && lba + nsect <= e->lba_start + e->lba_count) {
            if (write && !e->writable) return -1;
            return 0;
        }
    }
    return -1; /* EPERM */
}

/* ---- фиксированные low-memory зоны шима (ядро на 0x90000; зона 0x8C000
 *      свободна: под ядром, выше MBR-стека 0x7C00) ---- */
#define STUB_PA  0x8C000u     /* 16-бит shim код (< 512 байт) */
#define CTL_PA   0x8C200u     /* блок управления             */
#define DAP_PA   0x8C280u     /* DAP пакета INT13 AH=42h     */

/* Layout CTL: 0 drive | 1 status(FF=pending,0=ok,1=CF) | 2 err(AH) | 3 write */
#define C_DRV   (CTL_PA + 0)
#define C_STA   (CTL_PA + 1)
#define C_ERR   (CTL_PA + 2)
#define C_WR    (CTL_PA + 3)

/* Layout DAP: 0 size=16 | 1 zero | 2 count(u16) | 4 offset(u32) | 8 seg(u16)
 *             10 top(u16) | 12 lba_lo(u32) | 16 lba_hi(u32) — всего 20 байт */

__asm__(
".text\n.align 16\n.globl rm_stub_begin, rm_stub_end\n"
"rm_stub_begin:\n"
"   cli\n"
"   xorw %ax, %ax\n"
"   movw %ax, %ds\n"
"   movw %ax, %es\n"
"   movw %ax, %ss\n"                  /* ESP прежний (~0x1BF00) — валиден при ss=0 */
"   movb $0xFF, 0x8C201\n"            /* status = pending */
"   movb 0x8C200, %dl\n"              /* drive */
"   cmpb $0, 0x8C203\n"               /* write? */
"   je 1f\n"
"   movb $0x43, %ah\n"
"   jmp 2f\n"
"1: movb $0x42, %ah\n"
"2: leaw 0x8C280, %si\n"
"   int $0x13\n"
"   jc 3f\n"
"   movb $0, 0x8C201\n"
"   jmp 4f\n"
"3: movb $1, 0x8C201\n"
"   movb %ah, 0x8C202\n"
"4: ljmp $0x08, $0\n"                 /* [patch] rm_call пишет EA imm32:imm16 */
"rm_stub_end:\n");

extern char rm_stub_begin[], rm_stub_end[];

static uint32_t rm_patch_off(void)   /* смещение последней инструкции ljmp */
{
    uint32_t len = (uint32_t)(rm_stub_end - rm_stub_begin);
    for (uint32_t i = len; i-- > 0;)
        if ((uint8_t)rm_stub_begin[i] == 0xEA) return i;
    return 0xFFFFFFFFu;
}

static void tramp_install(void)
{
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)STUB_PA;
    uint32_t len = (uint32_t)(rm_stub_end - rm_stub_begin);
    for (uint32_t i = 0; i < len; i++) dst[i] = (uint8_t)rm_stub_begin[i];
}

/* rm_call(): PM32 -> lcall 16-bit stub -> ljmp .Lafter -> ret.
 * Адрес патча (STUB_PA+off_ljmp) лежит в статике g_patch_va (символ виден
 * из asm как _g_patch_va — cdecl name mangling для -m32 gcc). */
static uint32_t g_patch_va;          /* STUB_PA + off(ljmp) */

__asm__(
".section .text\n.globl rm_call\n"
"rm_call:\n"
"   pushfl\n"
"   pushl %ebx\n"
"   pushl %ecx\n"
"   pushl %edx\n"
"   pushl %esi\n"
"   leal .Lafter(%eip), %ecx\n"       /* адрес возврата в PM32 */
"   movl g_patch_va, %ebx\n"           /* gcc -m32: без нижнего подчёркивания */
"   movb $0xEA, (%ebx)\n"             /* ljmp ptr16:32 */
"   movl %ecx, 1(%ebx)\n"             /* off32 = .Lafter */
"   movw $0x08, 5(%ebx)\n"            /* sel32 = PM code */
"   lcall $0x28, $0x8C000\n"          /* 16-бит code селектор GDT M1 */
".Lafter:\n"
"   popl %esi\n"
"   popl %edx\n"
"   popl %ecx\n"
"   popl %ebx\n"
"   popfl\n"
"   ret\n");

void rm_call(void);

static void set_ctl(uint32_t lba, uint32_t nsect, uintptr_t buf_pa, int write)
{
    *(volatile uint8_t *)(uintptr_t)C_DRV = g_drive;
    *(volatile uint8_t *)(uintptr_t)C_WR  = (uint8_t)write;
    volatile uint8_t *dap = (volatile uint8_t *)(uintptr_t)DAP_PA;
    dap[0] = 16; dap[1] = 0;
    *(volatile uint16_t *)(uintptr_t)(DAP_PA + 2)  = (uint16_t)nsect;
    *(volatile uint32_t *)(uintptr_t)(DAP_PA + 4)  = (uint32_t)buf_pa;
    *(volatile uint16_t *)(uintptr_t)(DAP_PA + 8)  = 0;
    *(volatile uint16_t *)(uintptr_t)(DAP_PA + 10) = 0;
    *(volatile uint32_t *)(uintptr_t)(DAP_PA + 12) = lba;
    *(volatile uint32_t *)(uintptr_t)(DAP_PA + 16) = 0;
}

int32_t disk_io_raw(uint32_t lba, uint32_t nsect, void *buf, int write)
{
    uintptr_t pa = (uintptr_t)buf;
    if (pa >= 0x100000u) return -3;    /* ограничение трамплина M1: low memory */
    set_ctl(lba, nsect, pa, write);
    tramp_install();
    rm_call2();
    uint8_t st = *(volatile uint8_t *)(uintptr_t)C_STA;
    if (st == 0xFF) return -4;         /* stub не завершился */
    if (st != 0)    return -10 - *(volatile uint8_t *)(uintptr_t)C_ERR;
    return 0;
}

int32_t disk_read(uint32_t lba, uint32_t nsect, void *buf)
{
    if (!nsect || nsect > 64) return -2;
    return disk_io_raw(lba, nsect, buf, 0);
}

int32_t disk_write(uint32_t lba, uint32_t nsect, const void *buf)
{
    if (!nsect || nsect > 64) return -2;
    return disk_io_raw(lba, nsect, (void *)buf, 1);
}

void disk_init(uint8_t drive)
{
    g_drive = drive;
    uint32_t off = rm_patch_off();
    if (off == 0xFFFFFFFFu) return;    /* panic-лог печатает caller */
    g_patch_va = STUB_PA + off;
    tramp_install();
    /* init-LibOS получает весь диск read+write (политику раздаёт сам LibOS) */
    if (g_diskmap_count == 0) {
        g_diskmap[0].lba_start = 0;
        g_diskmap[0].lba_count = 1u << 24;
        g_diskmap[0].writable  = 1;
        g_diskmap[0].used      = 1;
        g_diskmap_count = 1;
    }
}
