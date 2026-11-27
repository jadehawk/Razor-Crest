/* RV32IM guest register spill helpers. The toolchain's libgcc implementation
 * is built with compressed instructions, which Dessen does not execute.
 * LLVM calls save via t0 and tail-calls restore. Frames are 16-byte aligned;
 * ra, s0..s11 have fixed offsets from the original stack pointer.
 */
.option norvc
.text
.macro alias name
.global \name
.type \name,@function
\name:
.endm

alias __riscv_save_12
    addi sp,sp,-64
    sw s11,12(sp)
    li t1,0
    j .Lsave8
alias __riscv_save_11
alias __riscv_save_10
alias __riscv_save_9
alias __riscv_save_8
    addi sp,sp,-64
    li t1,-16
.Lsave8:
    sw s10,16(sp)
    sw s9,20(sp)
    sw s8,24(sp)
    sw s7,28(sp)
    j .Lsave4
alias __riscv_save_7
alias __riscv_save_6
alias __riscv_save_5
alias __riscv_save_4
    addi sp,sp,-64
    li t1,-32
.Lsave4:
    sw s6,32(sp)
    sw s5,36(sp)
    sw s4,40(sp)
    sw s3,44(sp)
    sw s2,48(sp)
    sw s1,52(sp)
    sw s0,56(sp)
    sw ra,60(sp)
    sub sp,sp,t1
    jr t0
alias __riscv_save_3
alias __riscv_save_2
alias __riscv_save_1
alias __riscv_save_0
    addi sp,sp,-16
    sw s2,0(sp)
    sw s1,4(sp)
    sw s0,8(sp)
    sw ra,12(sp)
    jr t0

alias __riscv_restore_12
    lw s11,12(sp)
    addi sp,sp,16
alias __riscv_restore_11
alias __riscv_restore_10
alias __riscv_restore_9
alias __riscv_restore_8
    lw s10,0(sp)
    lw s9,4(sp)
    lw s8,8(sp)
    lw s7,12(sp)
    addi sp,sp,16
alias __riscv_restore_7
alias __riscv_restore_6
alias __riscv_restore_5
alias __riscv_restore_4
    lw s6,0(sp)
    lw s5,4(sp)
    lw s4,8(sp)
    lw s3,12(sp)
    addi sp,sp,16
alias __riscv_restore_3
alias __riscv_restore_2
alias __riscv_restore_1
alias __riscv_restore_0
    lw s2,0(sp)
    lw s1,4(sp)
    lw s0,8(sp)
    lw ra,12(sp)
    addi sp,sp,16
    ret
