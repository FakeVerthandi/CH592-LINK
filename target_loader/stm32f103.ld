ENTRY(loader_entry)

MEMORY {
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x400
}

SECTIONS {
    .loader : {
        KEEP(*(.vectors))
        LONG(loader_stop)
        *(.text .text.* .rodata .rodata.*)
        . = ALIGN(4);
    } > SRAM
    .unexpected : { *(.data* .bss* COMMON) } > SRAM
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment) }
    ASSERT(SIZEOF(.unexpected) == 0, "F103 loader must be freestanding")
}
