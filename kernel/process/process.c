#include "process.h"

#include "../exception/gate.h"
#include "../common/printk.h"
#include "../common/kprint.h"

/**
 * @brief 切换进程
 *
 * @param prev 上一个进程的pcb
 * @param next 将要切换到的进程的pcb
 * 由于程序在进入内核的时候已经保存了寄存器，因此这里不需要保存寄存器。
 * 这里切换fs和gs寄存器
 */

void __switch_to(struct process_control_block *prev, struct process_control_block *next)
{
    initial_tss[0].rsp0 = next->thread->rbp;
    set_TSS64(initial_tss[0].rsp0, initial_tss[0].rsp1, initial_tss[0].rsp2, initial_tss[0].ist1,
              initial_tss[0].ist2, initial_tss[0].ist3, initial_tss[0].ist4, initial_tss[0].ist5, initial_tss[0].ist6, initial_tss[0].ist7);

    __asm__ __volatile__("movq	%%fs,	%0 \n\t"
                         : "=a"(prev->thread->fs));
    __asm__ __volatile__("movq	%%gs,	%0 \n\t"
                         : "=a"(prev->thread->gs));

    __asm__ __volatile__("movq	%0,	%%fs \n\t" ::"a"(next->thread->fs));
    __asm__ __volatile__("movq	%0,	%%gs \n\t" ::"a"(next->thread->gs));

}

/**
 * @brief 内核init进程
 *
 * @param arg
 * @return ul 参数
 */
ul initial_kernel_thread(ul arg)
{
    kinfo("initial proc running...\targ:%#018lx", arg);
    return 1;
}

/**
 * @brief 进程退出时执行的函数
 *
 * @param code 返回码
 * @return ul
 */
ul do_exit(ul code)
{
    kinfo("thread_exiting..., code is %#018lx.", code);
    while (1)
        ;
}

/**
 * @brief 导出内核线程的执行引导程序
 *  目的是还原执行现场(在kernel_thread中伪造的)
 * 执行到这里时，rsp位于栈顶，然后弹出寄存器值
 * 弹出之后还要向上移动7个unsigned long的大小，从而弹出额外的信息（详见pt_regs）
 */

extern void kernel_thread_func(void);
__asm__ (
"kernel_thread_func:	\n\t"
"	popq	%r15	\n\t"
"	popq	%r14	\n\t"	
"	popq	%r13	\n\t"	
"	popq	%r12	\n\t"	
"	popq	%r11	\n\t"	
"	popq	%r10	\n\t"	
"	popq	%r9	\n\t"	
"	popq	%r8	\n\t"	
"	popq	%rbx	\n\t"	
"	popq	%rcx	\n\t"	
"	popq	%rdx	\n\t"	
"	popq	%rsi	\n\t"	
"	popq	%rdi	\n\t"	
"	popq	%rbp	\n\t"	
"	popq	%rax	\n\t"	
"	movq	%rax,	%ds	\n\t"
"	popq	%rax		\n\t"
"	movq	%rax,	%es	\n\t"
"	popq	%rax		\n\t"
"	addq	$0x38,	%rsp	\n\t"
/////////////////////////////////
"	movq	%rdx,	%rdi	\n\t"
"	callq	*%rbx		\n\t"
"	movq	%rax,	%rdi	\n\t"
"	callq	do_exit		\n\t"
);


/**
 * @brief 初始化内核进程
 *
 * @param fn 目标程序的地址
 * @param arg 向目标程序传入的参数
 * @param flags
 * @return int
 */

int kernel_thread(unsigned long (*fn)(unsigned long), unsigned long arg, unsigned long flags)
{
    struct pt_regs regs;
    memset(&regs, 0, sizeof(regs));

    // 在rbx寄存器中保存进程的入口地址
    regs.rbx = (ul)fn;
    // 在rdx寄存器中保存传入的参数
    regs.rdx = (ul)arg;

    regs.ds = KERNEL_DS;
    regs.es = KERNEL_DS;
    regs.cs = KERNEL_CS;
    regs.ss = KERNEL_DS;


    // 置位中断使能标志位
    regs.rflags = (1 << 9);

    // rip寄存器指向内核线程的引导程序
    regs.rip = (ul)kernel_thread_func;

    return do_fork(&regs, flags, 0, 0);
}

void process_init()
{


    initial_mm.pgd = (pml4t_t *)global_CR3;

    initial_mm.code_addr_start = memory_management_struct.kernel_code_start;
    initial_mm.code_addr_end = memory_management_struct.kernel_code_end;

    initial_mm.data_addr_start = (ul)&_data;
    initial_mm.data_addr_end = memory_management_struct.kernel_data_end;

    initial_mm.rodata_addr_start = (ul)&_rodata;
    initial_mm.rodata_addr_end = (ul)&_erodata;

    initial_mm.brk_start = 0;
    initial_mm.brk_end = memory_management_struct.kernel_end;

    initial_mm.stack_start = _stack_start;

    // 初始化进程和tss
    set_TSS64(initial_thread.rbp, initial_tss[0].rsp1, initial_tss[0].rsp2, initial_tss[0].ist1, initial_tss[0].ist2, initial_tss[0].ist3, initial_tss[0].ist4, initial_tss[0].ist5, initial_tss[0].ist6, initial_tss[0].ist7);

    initial_tss[0].rsp0 = initial_thread.rbp;
   

    // 初始化进程的循环链表
    list_init(&initial_proc_union.pcb.list);


    kernel_thread(initial_kernel_thread, 10, CLONE_FS | CLONE_FILES | CLONE_SIGNAL); // 初始化内核进程
    initial_proc_union.pcb.state = PROC_RUNNING;

    // 获取新的进程的pcb
    struct process_control_block *p = container_of(list_next(&current_pcb->list), struct process_control_block, list);

	// 切换到新的内核线程
    switch_proc(current_pcb, p);
}

/**
 * @brief fork当前进程
 *
 * @param regs 新的寄存器值
 * @param clone_flags 克隆标志
 * @param stack_start 堆栈开始地址
 * @param stack_size 堆栈大小
 * @return unsigned long
 */

unsigned long do_fork(struct pt_regs *regs, unsigned long clone_flags, unsigned long stack_start, unsigned long stack_size)
{
    struct process_control_block *tsk = NULL;


    // 获取一个物理页并在这个物理页内初始化pcb
    struct Page *pp = alloc_pages(ZONE_NORMAL, 1, PAGE_PGT_MAPPED | PAGE_ACTIVE | PAGE_KERNEL);


    tsk = (struct process_control_block *)phys_2_virt(pp->addr_phys);


    memset(tsk, 0, sizeof(*tsk));

    // 将当前进程的pcb复制到新的pcb内
    *tsk = *current_pcb;

    // 将进程加入循环链表
    list_init(&tsk->list);

    list_add(&initial_proc_union.pcb.list, &tsk->list);


    ++(tsk->pid);
    tsk->state = PROC_UNINTERRUPTIBLE;

    // 将线程结构体放置在pcb的后面
    struct thread_struct *thd = (struct thread_struct *)(tsk + 1);
    tsk->thread = thd;

    // 将寄存器信息存储到进程的内核栈空间的顶部
    memcpy((void *)((ul)tsk + STACK_SIZE - sizeof(struct pt_regs)), regs, sizeof(struct pt_regs));

    // 设置进程的内核栈
    thd->rbp = (ul)tsk + STACK_SIZE;
    thd->rip = regs->rip;
    thd->rsp = (ul)tsk + STACK_SIZE - sizeof(struct pt_regs);

    // 若进程不是内核层的进程，则跳转到ret from intr
    if (!(tsk->flags & PF_KTHREAD))
        thd->rip = regs->rip = (ul)ret_from_intr;

    tsk->state = PROC_RUNNING;

    return 0;
}