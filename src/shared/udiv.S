@ Unsigned 32-bit divide for the Cortex-M0 (no divide instruction), in place of libgcc's
@ __udivsi3 (266 B of unrolled code): a 32-step shift-subtract loop, 13 instructions.
@ __aeabi_uidivmod returns the quotient in r0 and the remainder in r1, as the EABI asks.
@ Up to ~300 cycles a divide (libgcc: ~50-130); no caller is in an ISR. Divide by zero
@ gives 0xFFFFFFFF, remainder the dividend (libgcc called __aeabi_idiv0, which returned).
@ Checked against libgcc under qemu-arm -cpu cortex-m0 on 3M cases, edge divisors included.
    .syntax unified
    .cpu cortex-m0
    .thumb
    .section .text.__udivsi3,"ax",%progbits
    .global __udivsi3
    .global __aeabi_uidiv
    .global __aeabi_uidivmod
    .thumb_func
__udivsi3:
    .thumb_func
__aeabi_uidiv:
    .thumb_func
__aeabi_uidivmod:
    movs r2, #0
    movs r3, #32
1:  lsls r0, r0, #1
    adcs r2, r2
    bcs 3f
    cmp r2, r1
    blo 2f
3:  subs r2, r2, r1
    adds r0, r0, #1
2:  subs r3, r3, #1
    bne 1b
    movs r1, r2
    bx lr
