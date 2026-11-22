ENTRY(loader_entry)

MEMORY {
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x600
}

SECTIONS {
    .loader : {
        KEEP(*(.vectors))
        LONG(loader_stop)
        *(.text .text.* .rodata .rodata.*)
        . = ALIGN(4);
    } > SRAM
    .unexpected : { *(.data* .bss* COMMON) } > SRAM
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .ARM.attributes) }
    ASSERT(SIZEOF(.unexpected) == 0, "Target loader must be freestanding")
}
