/* pci.c — enumeration и конфигурационный доступ к PCI (mechanism #1).
 * Protected resource: в M1 ядро предоставляет cfg-доступ всем модулям LibOS;
 * granular iomap на BAR/IRQ — этап M2+ (agent.md §4.2/§4.4).
 */
#include <stdint.h>
#include "port.h"

#define PCI_CFG_ADDR 0xCF8u
#define PCI_CFG_DATA 0xCFCu

static inline uint32_t cfg_addr(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg)
{
    return 0x80000000u | (bus << 16) | (dev << 8) | (fn << 3) | (reg & 0xFCu);
}

uint32_t pci_cfg_read(uint32_t bdf, uint32_t reg)
{
    outl(PCI_CFG_ADDR, cfg_addr(bdf >> 16, (bdf >> 8) & 31, bdf & 7, reg));
    uint32_t v = inl(PCI_CFG_DATA);
    uint32_t off = reg & 3;
    return v >> (off * 8);             /* byte-читаемый рег по смыслу вызывающего:
                                          возвращаем 32-бит слово со сдвигом */
}

void pci_cfg_write(uint32_t bdf, uint32_t reg, uint32_t val)
{
    outl(PCI_CFG_ADDR, cfg_addr(bdf >> 16, (bdf >> 8) & 31, bdf & 7, reg));
    outl(PCI_CFG_DATA, val << ((reg & 3) * 8));
}

/* Полный 32-битный read/write без сдвига (для BAR'ов) */
uint32_t pci_cfg_read32(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg)
{
    outl(PCI_CFG_ADDR, cfg_addr(bus, dev, fn, reg));
    return inl(PCI_CFG_DATA);
}

void pci_cfg_write32(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg, uint32_t val)
{
    outl(PCI_CFG_ADDR, cfg_addr(bus, dev, fn, reg));
    outl(PCI_CFG_DATA, val);
}

struct PciDev {
    uint32_t bdf;          /* bus<<16|dev<<8|fn */
    uint16_t vendor, device;
    uint8_t  classcode, subclass, progif, irqline;
    uint32_t bar[6];
};

#define PCI_MAX_DEV 24
static struct PciDev g_devs[PCI_MAX_DEV];
static int g_ndev;

int pci_enumerate(void)   /* возвращает число устройств */
{
    g_ndev = 0;
    for (uint32_t bus = 0; bus < 1; bus++)
        for (uint32_t dev = 0; dev < 32; dev++)
            for (uint32_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_cfg_read32(bus, dev, fn, 0);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF || vendor == 0) continue;
                if (g_ndev >= PCI_MAX_DEV) return g_ndev;
                struct PciDev *d = &g_devs[g_ndev++];
                d->bdf = (bus << 16) | (dev << 8) | fn;
                d->vendor = vendor;
                d->device = (uint16_t)(id >> 16);
                uint32_t revcls = pci_cfg_read32(bus, dev, fn, 8);
                d->classcode = (uint8_t)(revcls >> 24);
                d->subclass  = (uint8_t)(revcls >> 16);
                d->progif    = (uint8_t)(revcls >> 8);
                uint32_t lin = pci_cfg_read32(bus, dev, fn, 0x3C);
                d->irqline   = (uint8_t)(lin >> 24);
                for (int i = 0; i < 6; i++)
                    d->bar[i] = pci_cfg_read32(bus, dev, fn, 0x10 + i * 4);
                if (fn && !(id & 0x80)) break;      /* multifunction check упрощённый */
            }
    return g_ndev;
}

int pci_find(uint16_t vendor, uint16_t device, uint32_t *bdf_out, struct PciDev *info)
{
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i].vendor == vendor && g_devs[i].device == device) {
            if (bdf_out) *bdf_out = g_devs[i].bdf;
            if (info) *info = g_devs[i];
            return 0;
        }
    return -1;
}

int pci_find_class(uint8_t classcode, uint8_t subclass, uint32_t *bdf_out)
{
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i].classcode == classcode &&
            (subclass == 0xFF || g_devs[i].subclass == subclass)) {
            if (bdf_out) *bdf_out = g_devs[i].bdf;
            return 0;
        }
    return -1;
}

int pci_count(void) { return g_ndev; }
const struct PciDev *pci_get(int idx) { return (idx >= 0 && idx < g_ndev) ? &g_devs[idx] : 0; }

void pci_enable_bus_master(uint32_t bdf)
{
    uint32_t cmd = pci_cfg_read32(bdf >> 16, (bdf >> 8) & 31, bdf & 7, 4);
    cmd |= 5; /* IO space enable | bus master enable */
    pci_cfg_write32(bdf >> 16, (bdf >> 8) & 31, bdf & 7, 4, cmd);
}
