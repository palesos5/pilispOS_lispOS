; kernel_entry.asm — тонкий asm-мост: забирает boot-параметры из регистров
; (exokernel boot ABI: EAX=magic, EBX=load_pa, ECX=drives, EDX=drive), ставит
; стек и вызывает C-функцию kernel_entry по cdecl. Это единственный asm на M0.

global kernel_asm_entry
extern kernel_entry

section .text
kernel_asm_entry:
    ; M0-исправление: noreturn-ядро не должно зависеть от стека MBR (esp=0x7C00
    ; лежит прямо под нами и затирается push'ами shim). Ставим стек на 0x1C000
    ; (свободная зона после ядра 0x90000+~1KiB... нет: 0x1C000 < 0x90000 — это
    ; зона до ядра, выше L-области BIOS; безопасный диапазон 0x10000..0x80000).
    mov esp, 0x1C000
    ; cdecl: аргументы на стеке, справа налево
    push edx                ; arg4: drive
    push ecx                ; arg3: drives
    push ebx                ; arg2: load_pa
    push eax                ; arg1: magic
    call kernel_entry       ; noreturn
.hang:
    hlt
    jmp .hang               ; страховка (недостижимо)
