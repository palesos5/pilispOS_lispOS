/* mm.h — PMM экзядра (физический менеджер) + kernel heap.
 * Аудит утечек (agent.md): kalloc/kfree строго парные; kfree статистически
 * проверяет, что адрес лежит в выданном пуле (magic-заголовки слотов).
 */
#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include <stdint.h>

void     pmm_init(uint32_t mem_mb);          /* bitmap фреймов 4MB..mem_mb   */
uint32_t pmm_alloc_frame(void);              /* физ. адрес фрейма (4KiB) или 0 */
void     pmm_free_frame(uint32_t pa);
uint32_t pmm_free_frames(void);              /* статистика для serial-отчёта  */

void    *kalloc(uint32_t n);                 /* bump+слоты, никогда не NULL на QEMU-рамке */
void     kfree(void *p);                     /* с проверкой двойного free                     */
uint32_t kalloc_live(void);                  /* число живых аллокаций (анти-утечка тест)      */

#endif
