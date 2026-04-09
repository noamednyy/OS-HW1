#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
extern struct spinlock wait_lock;
extern struct proc proc[NPROC];

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


/*
 * co_yield(target_pid, value):
 *   Hand the CPU directly to target_pid, delivering value.
 *   The calling process sleeps on co_direct_chan(p->pid).
 *   The target is woken by setting it RUNNABLE; with CPUS=1 the
 *   scheduler picks it up immediately — effectively a direct handoff.
 *
 * Bootstrap:
 *   First call: target may not have called co_yield yet (sleeping on
 *   co_init_chan) or may not have started at all.  Handle both cases.
 *
 * Steady-state (both partners established):
 *   target is sleeping on co_direct_chan(target->pid).
 *   We set it RUNNABLE, deliver the value, then sleep ourselves on
 *   co_direct_chan(p->pid) via sched() (proper scheduler integration).
 *   The partner wakes us the same way next time it calls co_yield.
 */

static void*
co_init_chan(int pid)
{
  return (void*)((uint64)pid << 2);
}

static void*
co_direct_chan(int pid)
{
  return (void*)(((uint64)pid << 2) | 1);
}

uint64
sys_co_yield(void)
{
  int target_pid;
  int value;
  struct proc *p = myproc();
  struct proc *target = 0;
  struct proc *pp;

  argint(0, &target_pid);
  argint(1, &value);

  if(target_pid <= 0 || target_pid == p->pid)
    return -1;

  acquire(&wait_lock);                     // noff=1

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->pid == target_pid && pp->state != UNUSED){
      target = pp;
      break;
    }
  }

  if(target == 0 || target->killed){
    release(&wait_lock);
    return -1;
  }

  /*
   * Steady-state: target is sleeping on co_direct_chan.
   * Wake it (RUNNABLE) and suspend ourselves via sched().
   * The scheduler picks target up immediately (CPUS=1).
   */
  if(target->state == SLEEPING &&
     (target->chan == co_direct_chan(target->pid) ||
      target->chan == co_init_chan(target->pid))){

    acquire(&target->lock);
    if(target->killed ||
       (target->state != SLEEPING) ||
       (target->chan != co_direct_chan(target->pid) &&
        target->chan != co_init_chan(target->pid))){
      release(&target->lock);
      release(&wait_lock);
      return -1;
    }

    /* Deliver the value and make target runnable. */
    target->trapframe->a0 = value;
    target->state = RUNNABLE;
    release(&target->lock);               // noff=1

    /* Sleep on co_direct_chan(p->pid) until our partner wakes us. */
    acquire(&p->lock);                    // noff=2
    p->state = SLEEPING;
    p->chan  = co_direct_chan(p->pid);
    release(&wait_lock);                  // noff=1  (only p->lock held)
    sched();                              // → scheduler, which releases p->lock
    /* Resumed here by partner's co_yield setting us RUNNABLE.
     * Scheduler re-acquired p->lock before switching back to us. */
    p->chan = 0;
    release(&p->lock);                    // noff=0

    if(p->killed)
      return -1;
    return p->trapframe->a0;
  }

  /*
   * Bootstrap B: target not ready yet — sleep on co_init_chan
   * until target's first co_yield wakes us.
   */
  sleep(co_init_chan(p->pid), &wait_lock);
  release(&wait_lock);

  if(p->killed)
    return -1;
  return p->trapframe->a0;
}
