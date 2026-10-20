/* syscall.h — exokernel syscall ABI LispOS (этап M1, BIOS/x86 32-bit).
 * Вызов: int 0x30. Аргументы: EAX=номер, EBX/ECX/EDX=арг1..3. Возврат в EAX.
 * LibOS линкуется с ядром в один flat-binary на M1 (разделение адресного
 * пространства — этап M2+); поэтому вызов — прямой call kernel_syscall().
 */
#ifndef LIBOS_SYSCALL_H
#define LIBOS_SYSCALL_H

#include <stdint.h>

/* Номера — общие с include/exo_abi.h (единый экзокос-ABI). */
#include "exo_abi.h"

/* Kernel-internal реализация (kernel/tty.c, kernel/disk.c). */
int32_t kernel_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3);

#endif
