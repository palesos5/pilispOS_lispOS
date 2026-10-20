/* port.h — inline PIO-хелперы экзядра (protected resource iomap в M1 открыт
 * полностью; granular-права — этап M2+, см. agent.md §4.2). */
#ifndef KERNEL_PORT_H
#define KERNEL_PORT_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port)); }

static inline uint8_t inb(uint16_t port)
{ uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }

static inline void outw(uint16_t port, uint16_t v)
{ __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(port)); }

static inline uint16_t inw(uint16_t port)
{ uint16_t r; __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(port)); return r; }

static inline void outl(uint16_t port, uint32_t v)
{ __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(port)); }

static inline uint32_t inl(uint16_t port)
{ uint32_t r; __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(port)); return r; }

#endif
