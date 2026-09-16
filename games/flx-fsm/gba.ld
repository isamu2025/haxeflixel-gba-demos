OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
}

SECTIONS {
    .text : {
        KEEP(*(.crt0))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom

    .data : {
        __data_start__ = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > ewram AT > rom

    __data_load__ = LOADADDR(.data);

    .bss : {
        __bss_start__ = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end__ = .;
        . = ALIGN(4);
    } > ewram

    end = .;
    _end = .;
    __end__ = .;
}
