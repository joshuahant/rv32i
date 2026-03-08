OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS {
    . = 0x80000000;

    /* crt0 _start must be the very first thing executed */
    .text.init : { *(.text.init) }

    .text : { *(.text .text.*) }

    . = ALIGN(4);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4);
    .data : { *(.data .data.*) }

    . = ALIGN(4);
    .bss : {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        _bss_end = .;
    }

    /* 64 KiB stack immediately after BSS */
    . = ALIGN(16);
    _stack_bottom = .;
    . = . + 0x10000;
    _stack_top = .;
}
