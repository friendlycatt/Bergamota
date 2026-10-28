ENTRY(_start)
SECTIONS {
    . = 0x50000000;
    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    . = ALIGN(16384);
    test_end = .;
    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
