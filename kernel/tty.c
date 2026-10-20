/* tty.c — exokernel примитивы консоли для LibOS (этап M1).
 * Реализация syscall ABI из include/libos/syscall.h:
 *   TTY_WRITE: побайтовый вывод в COM1 (VGA-вывод — этап M2+).
 *   TTY_READ : polling PS/2 scan code set 1 -> ASCII, неблокирующий.
 * Аудит утечек: аллокаций нет (кольцевой буфер статический).
 */
#include <stdint.h>
#include "libos/syscall.h"

extern void serial_putc_local(char c); /* определим здесь же, без правки serial.c */

static void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }
static void com_putc(char c){ while((inb(0x3F8+5)&0x20)==0){} outb(0x3F8,(uint8_t)c); }
void serial_putc_local(char c){ com_putc(c); }

/* ---- PS/2 keyboard (порт 0x60), scan code set 1, базовая таблица ---- */
static const char kbd_ascii[128] = {
     0,  27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\r',  0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',  0,'\\','z','x','c','v',
    'b','n','m',',','.','/',  0,'*',  0,' ',  0 /*caps*/,0,0,0,0,0,0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

int32_t kernel_syscall(uint32_t n, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a3;
    switch (n) {
    case EX_SYS_TTY_WRITE: {
        const char *buf = (const char *)(uintptr_t)a1;
        uint32_t len = a2;
        for (uint32_t i = 0; i < len; i++) {
            if (buf[i] == '\n') com_putc('\r');
            com_putc(buf[i]);
        }
        return (int32_t)len;
    }
    case EX_SYS_TTY_READ: {
        char *buf = (char *)(uintptr_t)a1;
        uint32_t max = a2, got = 0;
        while (got < max) {
            if ((inb(0x64) & 1) == 0) break;          /* KBD status: нет данных */
            uint8_t sc = inb(0x60);
            if (sc & 0x80) continue;                   /* break code — пропускаем */
            char c = (sc < 128) ? kbd_ascii[sc] : 0;
            if (c == '\r') c = '\n';                   /* нормализуем EOL для Forth */
            if (!c) continue;
            buf[got++] = c;
        }
        return (int32_t)got;                           /* 0 = no data (неблок.) */
    }
    case EX_SYS_EXIT:
        for (;;) __asm__ volatile("hlt");
    default:
        return -1;                                     /* ENOSYS */
    }
}
