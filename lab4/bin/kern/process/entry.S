    .text
    .align 2
    .globl kernel_thread_entry
    .type kernel_thread_entry, @function

kernel_thread_entry:
    # s1 保存线程函数的参数
    mv a0, s1

    # s0 保存线程函数的地址
    jalr s0

    # 函数返回值已在 a0 中，作为退出码
    jal do_exit

    .size kernel_thread_entry, .-kernel_thread_entry