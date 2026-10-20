/* irq.c — PIC 8259 + IDT: protected resource "irq" (agent.md §4.2).
 * M1: обработчики регистрирует ядро для своих драйверов (timer/kbd);
 * upcall-маршрутизация в LibOS-приложения — этап M2+.
 * ISR: сохраняют счётчик и ack'ают PIC; тяжёлую работу делает polling
 * (IRQ_WAIT syscall) — детерминированно и без гонок на M1.
 */
#include <stdint.h>
#include "port.h"

#define PIC1_CMD  0x20u
#define PIC1_DATA 0x21u
#define PIC2_CMD  0xA0u
#define PIC2_DATA 0xA1u

static volatile uint32_t g_irq_count[16];

struct IdtEntry { uint16_t off_lo, sel; uint8_t zero, type; uint16_t off_hi; }
    __attribute__((packed));
static struct IdtEntry idt[256] __attribute__((aligned(16)));

void pic_init(void)
{
    outb(PIC1_CMD, 0x11); outb(PIC2_CMD, 0x11);       /* ICW1: init+cascade */
    outb(PIC1_DATA, 0x20); outb(PIC2_DATA, 0x28);     /* IRQ0.. -> int 0x20.. */
    outb(PIC1_DATA, 4);    outb(PIC2_DATA, 2);        /* cascade: IR2 / slave2 */
    outb(PIC1_DATA, 1);    outb(PIC2_DATA, 1);        /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0xFF & ~(1 << 0));                /* mask всё, кроме IRQ0 (timer) */
    outb(PIC2_DATA, 0xFF);                            /* mask всё (kbd опросом) */
}

void irq_enable(uint8_t irq)
{
    if (irq > 15) return;
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1u << (irq & 7)));
}

void irq_disable(uint8_t irq)
{
    if (irq > 15) return;
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1u << (irq & 7)));
}

uint32_t irq_taken(uint8_t irq) { return (irq <= 15) ? g_irq_count[irq] : 0; }

/* Сброс счётчика и проверка "было ли с тех пор" — семантика EX_SYS_IRQ_WAIT */
int irq_test_and_clear(uint8_t irq)
{
    if (irq > 15) return 0;
    if (g_irq_count[irq]) { g_irq_count[irq] = 0; return 1; }
    return 0;
}

/* generic EOI */
void irq_eoi(uint8_t irq)
{
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* --- 16 ISR-стабов: push irq; call isr_common (AT&T). Метки объявлены
 * .globl + .type function — на них ссылается C-таблица g_stubs[]. --- */
__asm__(
".section .text\n"
".globl isr_stub_0\n.type isr_stub_0,@function\nisr_stub_0: pushl $0;  jmp isr_common\n"
".globl isr_stub_1\n.type isr_stub_1,@function\nisr_stub_1: pushl $1;  jmp isr_common\n"
".globl isr_stub_2\n.type isr_stub_2,@function\nisr_stub_2: pushl $2;  jmp isr_common\n"
".globl isr_stub_3\n.type isr_stub_3,@function\nisr_stub_3: pushl $3;  jmp isr_common\n"
".globl isr_stub_4\n.type isr_stub_4,@function\nisr_stub_4: pushl $4;  jmp isr_common\n"
".globl isr_stub_5\n.type isr_stub_5,@function\nisr_stub_5: pushl $5;  jmp isr_common\n"
".globl isr_stub_6\n.type isr_stub_6,@function\nisr_stub_6: pushl $6;  jmp isr_common\n"
".globl isr_stub_7\n.type isr_stub_7,@function\nisr_stub_7: pushl $7;  jmp isr_common\n"
".globl isr_stub_8\n.type isr_stub_8,@function\nisr_stub_8: pushl $8;  jmp isr_common\n"
".globl isr_stub_9\n.type isr_stub_9,@function\nisr_stub_9: pushl $9;  jmp isr_common\n"
".globl isr_stub_10\n.type isr_stub_10,@function\nisr_stub_10: pushl $10; jmp isr_common\n"
".globl isr_stub_11\n.type isr_stub_11,@function\nisr_stub_11: pushl $11; jmp isr_common\n"
".globl isr_stub_12\n.type isr_stub_12,@function\nisr_stub_12: pushl $12; jmp isr_common\n"
".globl isr_stub_13\n.type isr_stub_13,@function\nisr_stub_13: pushl $13; jmp isr_common\n"
".globl isr_stub_14\n.type isr_stub_14,@function\nisr_stub_14: pushl $14; jmp isr_common\n"
".globl isr_stub_15\n.type isr_stub_15,@function\nisr_stub_15: pushl $15; jmp isr_common\n"
"isr_common:\n"
"   pushal\n"
"   pushl %esp\n"                     /* arg: struct PtRegs* */
"   call isr_handler\n"               /* gcc -m32: без нижнего подчёркивания */
"   addl $4, %esp\n"                  /* cdecl cleanup */
"   popal\n"
"   addl $4, %esp\n"                  /* убрать номер irq */
"   iretd\n");

struct PtRegs {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t irq;
    uint32_t eip, cs, eflags;
};

/* Таблица адресов стабов собираем явно (portability > трюков с метками). */
#define EXTSTUB(n) extern void *isr_stub_##n;
EXTSTUB(0)  EXTSTUB(1)  EXTSTUB(2)  EXTSTUB(3)
EXTSTUB(4)  EXTSTUB(5)  EXTSTUB(6)  EXTSTUB(7)
EXTSTUB(8)  EXTSTUB(9)  EXTSTUB(10) EXTSTUB(11)
EXTSTUB(12) EXTSTUB(13) EXTSTUB(14) EXTSTUB(15)
static void *g_stubs[16] = {
    (void*)isr_stub_0,  (void*)isr_stub_1,  (void*)isr_stub_2,  (void*)isr_stub_3,
    (void*)isr_stub_4,  (void*)isr_stub_5,  (void*)isr_stub_6,  (void*)isr_stub_7,
    (void*)isr_stub_8,  (void*)isr_stub_9,  (void*)isr_stub_10, (void*)isr_stub_11,
    (void*)isr_stub_12, (void*)isr_stub_13, (void*)isr_stub_14, (void*)isr_stub_15,
};

void isr_handler(struct PtRegs *r)
{
    uint32_t irq = r->irq;
    if (irq <= 15) {
        g_irq_count[irq]++;
        irq_eoi((uint8_t)irq);
    }
}

/* --- установка IDT: 16 IRQ + 0x30 syscall gate (не используется M1: прямой call) --- */
struct Idtr { uint16_t limit; uint32_t base; } __attribute__((packed));

static void idt_set_gate(uint8_t n, uint32_t handler, uint16_t sel, uint8_t type)
{
    idt[n].off_lo = (uint16_t)(handler & 0xFFFF);
    idt[n].sel    = sel;
    idt[n].zero   = 0;
    idt[n].type   = type;             /* 0x8E = int gate ring0 P=1 DPL0 */
    idt[n].off_hi = (uint16_t)(handler >> 16);
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0);
    for (int i = 0; i < 16; i++)
        idt_set_gate(0x20 + i, (uint32_t)(uintptr_t)g_stubs[i], 0x08, 0x8E);
    struct Idtr ptr = { sizeof(idt) - 1, (uint32_t)(uintptr_t)idt };
    __asm__ volatile("lidtl (%0)" :: "r"(&ptr));
}
