/* main.c — точка входа экзядра LispOS (этап M0, BIOS/x86 32-bit).
 * M0: инициализация COM1, вывод banner + BootInfo, остановка в hlt-цикле.
 * Память не аллоцируется (аудит утечек: 0 malloc/0 new — только статика/стек).
 */
#include <stdint.h>
#include "kernel/bootinfo.h"

void serial_init(void);
void serial_puts(const char *s);
void serial_put_hex(uint32_t v);
void serial_put_dec(uint32_t v);

static BootInfo g_bootinfo;

/* noreturn: kernel_entry вызывается из 32-битного shim загрузчика.
 * Параметры приходят в регистрах согласно exokernel boot ABI:
 *   EAX=magic, EBX=kernel_load_pa, ECX=drives_count, EDX=boot_drive.
 */
__attribute__((noreturn))
void kernel_entry(uint32_t magic, uint32_t load_pa,
                  uint32_t drives, uint32_t drive)
{
    /* 1. Serial console — главный канал отладки/тестов. */
    serial_init();

    /* 2. Проверка ABI-соглашения с загрузчиком. */
    if (magic != BOOT_MAGIC) {
        serial_puts("FATAL: bad boot magic\n");
        for (;;) __asm__ volatile("hlt");
    }

    g_bootinfo.magic         = magic;
    g_bootinfo.kernel_load_pa = load_pa;
    g_bootinfo.drives_count  = (uint16_t)drives;
    g_bootinfo.boot_drive    = (uint16_t)drive;
    g_bootinfo.mem_mb        = 0; /* TODO M1: INT 15h e820 (нужен trampoline в real mode) */

    /* 3. Banner + диагностика. */
    serial_puts("\n[LispOS exokernel] M0 OK\n");
    serial_puts("  boot magic     : "); serial_put_hex(g_bootinfo.magic);       serial_puts("\n");
    serial_puts("  kernel load pa : "); serial_put_hex(g_bootinfo.kernel_load_pa); serial_puts("\n");
    serial_puts("  boot drive     : "); serial_put_hex(g_bootinfo.boot_drive);   serial_puts("\n");
    serial_puts("  drives         : "); serial_put_dec(g_bootinfo.drives_count); serial_puts("\n");

    /* 4. Дальше — idle (прерываний ещё нет, PIC замаскирован обнулением? M1). */
    serial_puts("[LispOS] entering idle loop\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
