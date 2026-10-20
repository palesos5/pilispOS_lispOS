/* serial.c — COM1 (UART 16550) драйвер для отладочного вывода экзядра.
 * Разрешённое исключение agent.md: C в ядре до M2, с аудитом утечек.
 * Память не аллоцирует вообще (статический буфер только на стеке вызывающего).
 */
#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void)
{
    outb(COM1 + 1, 0x00);           /* disable interrupts */
    outb(COM1 + 3, 0x80);           /* DLAB on */
    outb(COM1 + 0, 0x0C);           /* divisor 12 => 9600 baud */
    outb(COM1 + 1, 0x00);           /* divisor high */
    outb(COM1 + 3, 0x03);           /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);           /* enable+clear FIFOs, trigger 14 */
    outb(COM1 + 4, 0x0B);           /* DTR|RTS|OUT2 */
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { } /* wait THR empty */
    outb(COM1 + 0, (uint8_t)c);
}

void serial_puts(const char *s)
{
    for (; *s; s++)
        serial_putc(*s);
}

/* Минимальный hex/dec вывод без malloc/vsnprintf (freestanding-safe). */
void serial_put_hex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[9];
    for (int i = 7; i >= 0; i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = 0;
    serial_puts(buf);
}

void serial_put_dec(uint32_t v)
{
    char buf[10];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) serial_putc(buf[i]);
}
