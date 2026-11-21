#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 2514231

void
schedule(void)
{
    bool intr_flag;
    list_entry_t *last, *le;
    struct proc_struct *next = NULL;

    local_intr_save(intr_flag);

    current->need_resched = 0;

    // idleproc 不在普通进程链表中，从链表头开始查找。
    // 其他线程从自己的下一个位置开始查找。
    last = (current == idleproc)
           ? &proc_list
           : &current->list_link;
    le = last;

    do {
        le = list_next(le);

        // 链表头不是进程，不能转换成 proc_struct。
        if (le != &proc_list) {
            struct proc_struct *proc = le2proc(le, list_link);

            if (proc->state == PROC_RUNNABLE) {
                next = proc;
                break;
            }
        }
    } while (le != last);

    // 没有可运行线程时，由空闲线程保底。
    if (next == NULL) {
        next = idleproc;
    }

    next->runs++;
    proc_run(next);

    local_intr_restore(intr_flag);
}


