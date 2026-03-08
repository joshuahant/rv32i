    .section .text.init
    .global _start
_start:
    /* Set up stack */
    la      sp, _stack_top

    /* Zero BSS (required by C/C++ for global/static variables) */
    la      t0, _bss_start
    la      t1, _bss_end
.Lbss_loop:
    bgeu    t0, t1, .Lbss_done
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       .Lbss_loop
.Lbss_done:

    /* Call main(); return value in a0 */
    call    main

    /* Signal pass/fail via tohost.
     * riscv-tests convention:
     *   write 1        -> PASS
     *   write (code<<1)|1  -> FAIL with exit code */
    bnez    a0, .Lfail
    li      a0, 1
    j       .Lwrite
.Lfail:
    slli    a0, a0, 1
    ori     a0, a0, 1
.Lwrite:
    la      t0, tohost
    sw      a0, 0(t0)
.Lspin:
    j       .Lspin

    /* tohost / fromhost — the simulator scans the ELF symbol table for these */
    .section .data
    .align  2
    .global tohost
tohost:
    .word   0
    .global fromhost
fromhost:
    .word   0
