.arm
.section .crt0, "ax"
.global _start
_start:
    b 1f
    .fill 156, 1, 0         @ Header logo placeholder for gbafix
    .ascii "FLXFSM      "   @ Game Title (12 chars)
    .ascii "FSM1"           @ Game Code (4 chars)
    .ascii "01"             @ Maker Code (2 chars)
    .byte 0x96              @ Fixed value
    .byte 0x00              @ Main unit code
    .byte 0x00              @ Device type
    .fill 7, 1, 0           @ Reserved
    .byte 0x00              @ Software version
    .byte 0x00              @ Complement check (fixed by gbafix)
    .short 0x0000           @ Checksum (fixed by gbafix)

1:
    @ Set stack pointer to end of IWRAM
    ldr sp, =0x03007F00

    @ Copy initialized .data from ROM to EWRAM
    ldr r0, =__data_start__
    ldr r1, =__data_end__
    ldr r2, =__data_load__
10:
    cmp r0, r1
    bge 20f
    ldr r3, [r2], #4
    str r3, [r0], #4
    b 10b

20:
    @ Zero out .bss in EWRAM
    ldr r0, =__bss_start__
    ldr r1, =__bss_end__
    mov r2, #0
30:
    cmp r0, r1
    bge 40f
    str r2, [r0], #4
    b 30b

40:
    @ Call C main
    ldr r0, =main
    bx r0

50:
    b 50b
