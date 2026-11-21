#include <mmu.h>
#include <memlayout.h>

    .section .text,"ax",%progbits
    .globl kern_entry
kern_entry:
    # a0: hartid
    # a1: dtb physical address
    # save hartid and dtb address
    la t0, boot_hartid
    sd a0, 0(t0)
    la t0, boot_dtb
    sd a1, 0(t0)
    
    # t0 := 三级页表的虚拟地址
    lui     t0, %hi(boot_page_table_sv39)
    # t1 := 0xffffffff40000000 即虚实映射偏移量
    li      t1, 0xffffffffc0000000 - 0x80000000
    # t0 减去虚实映射偏移量 0xffffffff40000000，变为三级页表的物理地址
    sub     t0, t0, t1
    # t0 >>= 12，变为三级页表的物理页号
    srli    t0, t0, 12

    # t1 := 8 << 60，设置 satp 的 MODE 字段为 Sv39
    li      t1, 8 << 60
    # 将刚才计算出的预设三级页表物理页号附加到 satp 中
    or      t0, t0, t1
    # 将算出的 t0(即新的MODE|页表基址物理页号) 覆盖到 satp 中
    csrw    satp, t0
    # 使用 sfence.vma 指令刷新 TLB
    sfence.vma
    # Switch the PC and stack to the high mapping before removing the low alias.
    lui sp, %hi(bootstacktop)
    lui t0, %hi(.Lhigh_entry)
    addi t0, t0, %lo(.Lhigh_entry)
    jr t0

.Lhigh_entry:
    la t0, boot_page_table_sv39
    sd zero, 16(t0) # Drop the temporary identity mapping in VPN[2] slot 2.
    sfence.vma
    tail kern_init

.section .data
    # .align 2^12
    .align PGSHIFT
    .global bootstack
bootstack:
    .space KSTACKSIZE
    .global bootstacktop
bootstacktop:

.section .data
    # 由于我们要把这个页表放到一个页里面，因此必须 12 位对齐
    .align PGSHIFT
    .global boot_page_table_sv39
# 分配 4KiB 内存给预设的三级页表
boot_page_table_sv39:
    .zero 8 * 2
    # The first instruction after enabling paging still uses its low address.
    .quad (0x80000 << 10) | 0xcf # 0x80000000 -> 0x80000000, 1 GiB.
    .zero 8 * (511 - 3)
    .quad (0x80000 << 10) | 0xcf # 0xffffffffc0000000 -> 0x80000000, 1 GiB.

    .global boot_hartid
boot_hartid:
    .quad 0
    .global boot_dtb
boot_dtb:
    .quad 0
