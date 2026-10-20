/* mm.c — PMM (bitmap физических фреймов) + kernel heap с аудитом утечек.
 *
 * Обнаружение памяти: MBR-загрузчик собирает INT 15h E820 в real mode ДО
 * перехода в PM и кладёт верх доступной RAM (в МБ, floor) по физ адресу
 * 0x7DB0 (MBR data area). Ядро читает подсказку; fallback 128 МБ.
 *
 * Аудит утечек (agent.md §правила): каждый блок kalloc получает заголовок
 * {magic,size,state}; kfree проверяет magic и double-free; live-счётчик
 * доступен автотесту (после alloc/free цикла обязан вернуться к 0).
 */
#include <stdint.h>
#include "kernel/mm.h"

static volatile uint32_t g_mem_mb;

void pmm_detect_memory(void)
{
    volatile uint32_t *hint = (volatile uint32_t *)0x7DB0;
    uint32_t top_mb = *hint;
    if (top_mb < 16 || top_mb > 4096) top_mb = 128; /* QEMU default fallback */
    g_mem_mb = top_mb;
}

/* ---------------- PMM bitmap ---------------- */
#define FRAME_SIZE 4096u
static uint32_t *bm;
static uint32_t  frames_total;
static uint32_t  frames_free;

void *kalloc(uint32_t n); /* из heap-ниже; pmm_init зовётся после kheap_init */

void pmm_init(uint32_t mem_mb)
{
    frames_total = (mem_mb * 1024u * 1024u) / FRAME_SIZE;
    uint32_t words = (frames_total + 31u) / 32u;
    bm = (uint32_t *)(uintptr_t)kalloc(words * 4u);
    for (uint32_t i = 0; i < words; i++) bm[i] = ~0u;   /* всё занято */
    uint32_t first_free = (16u * 1024u * 1024u) / FRAME_SIZE; /* ниже 16MB не трогаем */
    frames_free = 0;
    for (uint32_t f = first_free; f < frames_total; f++) {
        bm[f >> 5] &= ~(1u << (f & 31));
        frames_free++;
    }
}

uint32_t pmm_alloc_frame(void)
{
    uint32_t words = (frames_total + 31u) / 32u;
    for (uint32_t w = 0; w < words; w++) {
        if (!bm[w]) continue;
        uint32_t bit = 0, x = bm[w];
        while (!(x & 1u)) { x >>= 1; bit++; }
        uint32_t f = w * 32u + bit;
        if (f >= frames_total) return 0;                 /* хвост bitmap */
        bm[w] &= ~(1u << bit);
        frames_free--;
        return f * FRAME_SIZE;
    }
    return 0;
}

void pmm_free_frame(uint32_t pa)
{
    uint32_t f = pa / FRAME_SIZE;
    if (f >= frames_total) return;
    if (!(bm[f >> 5] & (1u << (f & 31)))) return;        /* уже свободен */
    bm[f >> 5] |= (1u << (f & 31));
    frames_free++;
}

uint32_t pmm_free_frames(void) { return frames_free; }
uint32_t kmem_mb(void)          { return g_mem_mb; }

/* ---------------- kernel heap: bump + free-list ----------------
 * Пул резервируется линкером (.kheap секция BSS, 8 MiB).
 * Блок = Hdr{magic,size,state,next_free} + данные. next_free валиден только
 * когда state==FREE. Ошибки (чужой адрес / double free) -> счётчик free_errs.
 */
#define HEAP_MAGIC      0x4B484C4Fu /* 'KHLN' */
#define HDR_STATE_ALLOC 0xA110u
#define HDR_STATE_FREE  0xF4EEu

typedef struct Hdr {
    uint32_t magic;
    uint32_t size;       /* всего блока вместе с заголовком */
    uint32_t state;
    struct Hdr *next_free;
} Hdr;

extern unsigned char _kheap_start[];
extern unsigned char _kheap_end[];
static unsigned char *hp;
static Hdr *freelist;
static uint32_t live_cnt, free_errs;

void kheap_init(void)
{
    hp = (unsigned char *)(((uintptr_t)_kheap_start + 15u) & ~15u);
    freelist = 0;
    live_cnt = 0;
    free_errs = 0;
}

void *kalloc(uint32_t n)
{
    if (!n) n = 1;
    uint32_t need = ((uint32_t)sizeof(Hdr) + n + 15u) & ~15u;

    /* first-fit по free-list */
    Hdr **pp = &freelist;
    while (*pp) {
        Hdr *cur = *pp;
        if (cur->size >= need) {
            *pp = cur->next_free;
            if (cur->size >= need + sizeof(Hdr) + 16u) { /* отщепить остаток */
                Hdr *rest = (Hdr *)((unsigned char *)cur + need);
                rest->magic = HEAP_MAGIC;
                rest->state = HDR_STATE_FREE;
                rest->size  = cur->size - need;
                rest->next_free = freelist;
                freelist = rest;
                cur->size = need;
            }
            cur->state = HDR_STATE_ALLOC;
            live_cnt++;
            return (unsigned char *)cur + sizeof(Hdr);
        }
        pp = &cur->next_free;
    }

    /* bump из пула */
    if ((uintptr_t)hp + need > (uintptr_t)_kheap_end) return 0;
    Hdr *h = (Hdr *)hp;
    h->magic = HEAP_MAGIC;
    h->size  = need;
    h->state = HDR_STATE_ALLOC;
    h->next_free = 0;
    hp += need;
    live_cnt++;
    return (unsigned char *)h + sizeof(Hdr);
}

void kfree(void *p)
{
    if (!p) return;
    Hdr *h = (Hdr *)((unsigned char *)p - sizeof(Hdr));
    if (h->magic != HEAP_MAGIC)   { free_errs++; return; }
    if (h->state == HDR_STATE_FREE) { free_errs++; return; }
    h->state = HDR_STATE_FREE;
    live_cnt--;
    h->next_free = freelist;
    freelist = h;
}

uint32_t kalloc_live(void)  { return live_cnt; }
uint32_t kfree_errors(void) { return free_errs; }
