/* exo_abi.h — единый экзокос-ABI LispOS (общие константы ядро<->LibOS).
 * Модель (agent.md §2): экзядро только мультиплексирует protected resources;
 * все абстракции (ФС, сеть, графика, звук, USB) — LibOS-библиотеки, которым
 * ядро выдаёт ПРАВА на ресурсы.
 */
#ifndef EXO_ABI_H
#define EXO_ABI_H

#include <stdint.h>

/* ---------- syscall номера (int 0x30 / прямой call kernel_syscall) ---------- */
#define EX_SYS_TTY_WRITE     1  /* ebx=buf ecx=len -> eax=написано                */
#define EX_SYS_TTY_READ      2  /* ebx=buf ecx=max -> eax=прочитано (неблок.)     */
#define EX_SYS_DISK_READ     3  /* ebx=lba ecx=nsect edx=kbuf -> eax=0/err        */
#define EX_SYS_EXIT          4  /* ebx=code                                       */
#define EX_SYS_DISK_WRITE    5  /* ebx=lba ecx=nsect edx=kbuf -> eax=0/err        */
#define EX_SYS_KMALLOC       6  /* ebx=size -> eax=pa                             */
#define EX_SYS_KFREE         7  /* ebx=pa                                         */
#define EX_SYS_PCI_CFG_READ  8  /* ebx=(bus<<16)|(dev<<8)|fn ecx=reg -> eax=val   */
#define EX_SYS_PCI_CFG_WRITE 9  /* eax: как read + edx=value                      */
#define EX_SYS_IRQ_ENABLE   10  /* ebx=irq(0..15)                                 */
#define EX_SYS_IRQ_DISABLE  11  /* ebx=irq                                        */
#define EX_SYS_IRQ_WAIT     12  /* ebx=irq -> eax=1 сработало (polling PIC)       */
#define EX_SYS_TIMER_TICKS  13  /* -> eax: тики PIT с загрузки                    */
#define EX_SYS_TIMER_SLEEP  14  /* ebx=тики (~18.2/s)                             */
#define EX_SYS_FB_INFO      15  /* -> eax=fb_base_pa ebx=w ecx=h edx=bpp          */
#define EX_SYS_PORT_IN      16  /* ebx=port ecx=1|2|4 -> eax=value                */
#define EX_SYS_PORT_OUT     17  /* ebx=port ecx=size edx=value                    */
#define EX_SYS_GET_MEMINFO  18  /* -> eax=mem_mb                                  */

/* ---------- таблица прав на protected resources (diskmap, ExOS Aegis) -------
 * Ядро проверяет каждую операцию DISK_READ/WRITE по таблице разрешённых
 * диапазонов секторов. Инициализируется при загрузке модуля LibOS.
 */
typedef struct {
    uint32_t lba_start;
    uint32_t lba_count;
    uint8_t  writable;
    uint8_t  used;
} DiskMapEntry;

#define DISKMAP_MAX 8
extern DiskMapEntry g_diskmap[DISKMAP_MAX];
extern int          g_diskmap_count;

int diskmap_check(uint32_t lba, uint32_t nsect, int write); /* 0 ok, -1 EPERM */

#endif
