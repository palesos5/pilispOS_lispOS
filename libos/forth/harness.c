/* harness.c — host-side автотест Forth-ядра LibOS (без QEMU).
 * Компилируется вместе с forth.c на хосте (-m32), подменяя kernel_syscall()
 * терминалом: вывод в stdout, ввод — из файла argv[1] (пакетный режим) или stdin.
 * Так тесты M1 гоняются быстро и детерминированно.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "libos/syscall.h"

extern void forth_main(void);

static char script_buf[65536]; static size_t script_len, script_pos;
static int batch_mode;

int32_t kernel_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a3;
    switch (n) {
    case EX_SYS_TTY_WRITE:
        fwrite((const void *)(uintptr_t)a1, 1, a2, stdout);
        fflush(stdout);
        return (int32_t)a2;
    case EX_SYS_TTY_READ: {
        char *b = (char *)(uintptr_t)a1;
        if (batch_mode) {
            /* конец скрипта => EOF как '\n', затем счётчик: после 8 пустых
             * переводов строки считаем что REPL залип -> выход (защита от зависания) */
            static int eof_nl;
            if (script_pos >= script_len) {
                if (++eof_nl > 8) { printf("\n[harness] EOF without BYE\n"); _exit(3); }
                b[0] = '\n'; return 1;
            }
            eof_nl = 0;
            b[0] = script_buf[script_pos++];
            return 1;
        }
        ssize_t r = read(0, b, 1);
        return (int32_t)(r == 1 ? 1 : 0);
    }
    case EX_SYS_EXIT: _exit(0);
    default: return -1;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        script_len = fread(script_buf, 1, sizeof(script_buf) - 1, f);
        fclose(f);
        if (script_len == 0 || script_buf[script_len-1] != '\n')
            script_buf[script_len++] = '\n';
        script_buf[script_len] = 0;
        batch_mode = 1;
    }
    forth_main();
    return 0;
}
