/* linker.ld — экзядро LispOS, BIOS/x86 32-bit.
 * Линкуемся на физический 0x90000 (адрес загрузки MBR-ом). Пагинация на M0 выключена:
 * cr0.PE=1 без PG => линейные адреса = физические.
 */
ENTRY(kernel_asm_entry)

SECTIONS
{
    . = 0x90000;

    /* Magic 'LSPK' в первых байтах — проверяется загрузчиком до прыжка. */
    .head 0x90000 : {
        LONG(0x4B50534C)   /* 'LSPK' little-endian */
        LONG(0)            /* reserved: checksum/size (M1) */
    }

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }

    .data : { *(.data*) }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame*)      /* ядру не нужен C++/unwind инфо */
        *(.comment)
    }
}
