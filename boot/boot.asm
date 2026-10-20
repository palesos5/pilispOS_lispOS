; boot.asm — MBR-загрузчик LispOS (BIOS/x86, этап M0)
; Цепочка: BIOS -> 0x7C00 -> COM1 init -> A20 -> INT13h LBA-чтение ядра в 0x90000
;         -> GDT -> protected mode -> jmp kernel.
; Ядро линкуется на физ. 0x90000; пагинация на M0 выключена (linear == physical).
; BootInfo передаётся регистрами (exokernel boot ABI):
;   EAX='LSPK', EBX=kernel_load_pa(0x90000), ECX=drives_count, EDX=boot_drive.
;
; Исправления после отладки QEMU 7.2 (M0):
; 1) Чтение ядра переведено на INT 13h AH=42h (LBA): CHS-форма с al=64 выходила за
;    пределы дорожки (18 секторов/дорожка) и возвращала CF -> "DISK READ ERROR".
; 2) GDT-дескрипторы записаны явными dq с полным limit=0xFFFFFFFF (старая dw/db-форма
;    теряла старшие байты limit -> #GP(0) сразу после перехода в PM).
; 3) Дальний прыжок в PM — корректная форма jmp dword 0x08:pm_entry (селектор+смещение);
;    метка pm_entry лежит ДО данных и error-путей, её смещение < 512.
; 4) В PM-shim строки адресуются как [ORG-смещение + ESP(=0x7C00)] — nasm выдаёт для
;    меток чистое ORG-смещение, базу 0x7C00 добавляем в рантайме.
; 5) Баннер MBR пишется в COM1 из real mode + дублируется в PM (тесты читают serial).

[BITS 16]
[ORG 0x7C00]

KERNEL_SEG  equ 0x9000        ; сегмент загрузки ядра: 0x9000*16 = 0x90000
KERNEL_SECT equ 64            ; M0: 64 сектора (32 KiB), начиная с LBA1
KERNEL_LBA  equ 1             ; образ: LBA0=MBR, LBA1..=ядро

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [DriveNum], dl

    ; --- COM1 init 9600 8N1 + баннер (serial — канал автотестов) ---
    call com1_init
    mov si, msg_boot
    call serial_print

    ; --- A20 gate: INT 15h AX=2401 best-effort + fastum A20 (порт 0x92) ---
    sti
    mov ax, 0x2401
    int 0x15
    in  al, 0x92
    or  al, 2
    out 0x92, al              ; alt-PIN control: bit1 = A20 on

    ; --- Reset disk ---
    xor ah, ah
    mov dl, [DriveNum]
    int 0x13

    ; --- CHS load kernel -> ES:BX = 0x9000:0000 (физ. 0x90000) ---
    ; QEMU-диск: 16 heads x 63 sectors => head1/sec1 = LBA16, ровно после MBR-Geometry.
    ; 64 сектора вписываются в дорожку (63+sec1..), переполнение головы обрабатывает BIOS.
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx
    mov ah, 0x02              ; read sectors
    mov al, KERNEL_SECT
    mov ch, 0                 ; cylinder 0
    mov cl, 1                 ; sector 1
    mov dh, 1                 ; head 1
    mov dl, [DriveNum]
    int 0x13
    jc  disk_err

    ; --- переход в protected mode ---
    cli
    lgdt [gdtr]               ; rel16-адрес внутри MBR (ds=0)
    mov eax, cr0
    or  eax, 1                ; PE=1, PG=0 => linear == physical
    mov cr0, eax
    jmp dword 0x08:pm_entry   ; far jump: sel 0x08 (code ring0), offset pm_entry

; ---------------- 32-bit shim (всё ещё внутри MBR-сектора) ----------------
[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00           ; стек: растёт вниз от конца MBR-страницы

    ; Дубль баннера "PM entered" в COM1. msg_pm — ORG-смещение; база 0x7C00 = ESP.
    xor esi, esi
    lea esi, [esi + msg_pm]
    add esi, esp
.pmprint:
    lodsb
    cmp al, 0
    je  .done32
    mov edx, 0x3F8            ; COM1 THR
    out dx, al
.pmwait:
    mov edx, 0x3FD            ; COM1 LSR
    in  al, dx
    test al, 0x20             ; THR empty?
    jz  .pmwait
    jmp .pmprint
.done32:

    ; BootInfo в регистрах (см. шапку файла)
    mov eax, 0x4B50534C       ; 'LSPK'
    mov ebx, KERNEL_SEG * 16  ; 0x90000
    movzx edx, byte [DriveNum]
    mov ecx, 1                ; drives_count (M0: один диск)
    jmp 0x90000               ; entry ядра = начало flat-бинарника

; ---------------- error paths / подпрограммы real mode ----------------
[BITS 16]
halt:
    hlt
    jmp halt

disk_err:
    mov si, msg_disk
    call serial_print
    jmp halt

; com1_init: 9600 8N1 (divisor 12), MCR DTR|RTS|OUT2.
com1_init:
    push ax
    push dx
    mov dx, 0x3F9             ; LCR
    mov al, 0x80              ; DLAB=1
    out dx, al
    mov dx, 0x3F8             ; divisor low
    mov al, 0x0C              ; 12 => 9600 baud
    out dx, al
    mov dx, 0x3F9             ; LCR
    mov al, 0x03              ; 8N1, DLAB off
    out dx, al
    mov dx, 0x3FB             ; MCR
    mov al, 0x0B              ; DTR|RTS|OUT2
    out dx, al
    pop dx
    pop ax
    ret

; serial_print: строка DS:[SI] в COM1 до 0-терминатора (polling THR empty).
serial_print:
    lodsb
    cmp al, 0
    je .done
    push ax
    push dx
    mov dx, 0x3F8
    out dx, al
.sp_wait:
    mov dx, 0x3FD
    in  al, dx
    test al, 0x20             ; THR empty?
    jz  .sp_wait
    pop dx
    pop ax
    jmp serial_print
.done:
    ret

; ---------------- data ----------------
msg_boot db "LispOS MBR loading...", 13, 10, 0
msg_disk db 13, 10, "DISK READ ERROR", 13, 10, 0
msg_pm   db "MBR: PM entered, jumping to kernel@0x90000", 13, 10, 0

DriveNum db 0

align 4
; LBA-пакет INT 13h AH=42h: size=16, count, addr_low/mid/high(seg), lba(low/high)
lba_packet:
    .size   dw 16
    .count  dw KERNEL_SECT
    .offset dd 0              ; ES:BX задаются динамически (ES=0x9000, BX=0)
    .seg    dw 0x9000
    .lba_low  dd KERNEL_LBA
    .lba_high dd 0

gdtr:
    dw gdt_end - gdt_start - 1
    dd gdt_start              ; [ORG] уже даёт линейный адрес (0x7C00+смещение)

gdt_start:
    dq 0                                  ; 0x00: null
    dq 0x00CF9A000000FFFF                  ; 0x08: code ring0, limit=0xFFFFFFFF (G=1,B=1)
    dq 0x00CF92000000FFFF                  ; 0x10: data ring0, limit=0xFFFFFFFF
gdt_end:

times 510-($-$$) db 0
db 0x55, 0xAA
