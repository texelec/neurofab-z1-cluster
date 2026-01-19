/* Z1 Onyx Bootloader - SRAM Linker Script
 * Based on Raspberry Pi encrypted bootloader example
 * 
 * Runs bootloader from high SRAM (0x20070000) to avoid RP2350-E10 errata
 * This allows partition table to be embedded without address conflicts
 */

MEMORY
{
    /* Use all available SRAM (520KB total) - bootloader needs space! */
    SRAM(rwx) : ORIGIN = 0x20000000, LENGTH = 520K
    
    /* Flash for boot2 stage only */
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 256
    
    /* Scratch areas at end of SRAM */
    SCRATCH_X(rwx) : ORIGIN = 0x20082000, LENGTH = 4K
    SCRATCH_Y(rwx) : ORIGIN = 0x20083000, LENGTH = 4K
}

ENTRY(_entry_point)

SECTIONS
{
    /* Boot2 stage must be first in flash */
    .boot2 : {
        __boot2_start__ = .;
        KEEP (*(.boot2))
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ == 256,
        "ERROR: boot2 must be 256 bytes")

    /* All code runs from SRAM */
    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.reset))
        
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        
        *(.init)
        *(.fini)
        
        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP (*(.preinit_array))
        __preinit_array_end = .;
        
        . = ALIGN(4);
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;
        
        . = ALIGN(4);
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP (*(.fini_array))
        __fini_array_end = .;
        
        *(.eh_frame*)
        . = ALIGN(4);
    } > SRAM

    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > SRAM

    __exidx_start = .;
    .ARM.exidx : {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > SRAM
    __exidx_end = .;

    .data : {
        __data_start__ = .;
        *(vtable)
        *(.data*)
        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        __data_end__ = .;
    } > SRAM

    .bss : {
        . = ALIGN(4);
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > SRAM

    .heap (NOLOAD): {
        __end__ = .;
        end = __end__;
        *(.heap*)
        . = ORIGIN(SRAM) + LENGTH(SRAM);
        __HeapLimit = .;
    } > SRAM

    .stack1_dummy (NOLOAD): {
        *(.stack1*)
    } > SCRATCH_X

    .stack_dummy (NOLOAD): {
        *(.stack*)
    } > SCRATCH_Y

    __StackTop = ORIGIN(SRAM) + LENGTH(SRAM);
    __StackLimit = __StackTop - SIZEOF(.stack_dummy);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackOneBottom;
    PROVIDE(__stack = __StackTop);

    .binary_info : {
        __binary_info_start = .;
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
        __binary_info_end = .;
    } > SRAM

    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y

    /DISCARD/ : {
        *(.note.GNU-stack)
    }

    ASSERT(__StackLimit >= __HeapLimit, "SRAM overflow!")
}
