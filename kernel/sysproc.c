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

static void*
co_sleep_chan(int pid)
{
  return (void*)(((uint64)pid << 1));
}

static void*
co_direct_chan(int pid)
{
  return (void*)((((uint64)pid << 1) | 1));
}

static void
co_resume_cleanup(struct proc *p)
{
  p->chan = 0;

  // In the direct-handoff path, the yielding partner may resume us
  // while our p->lock is still held across the switch.
  // However, because this implementation mixes two rendezvous modes
  // (sleep-based and direct-handoff based) without adding new fields to
  // struct proc, we release p->lock here only if it is actually held.
  // This keeps the code safe in the assignment's single-CPU setting and
  // avoids mismatched release() panics.
  if(holding(&p->lock))
    release(&p->lock);
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

  acquire(&wait_lock);

  // Find the target process.
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
   * Direct handoff implementation for co_yield.
   *
   * Design scope:
   * - Works in the assignment's required single-CPU configuration.
   * - Intended for cooperative ping-pong between two processes.
   * - Does not attempt to fully handle all races among multiple unrelated
   *   processes concurrently yielding to the same target.
   *
   * We distinguish between two waiting modes using two different channels:
   *
   * 1. co_sleep_chan(pid):
   *    The process is blocked inside sleep(..., &wait_lock).
   *    If we hand off directly to such a process, it resumes inside sleep(),
   *    and sleep() itself will clear chan and release its own lock.
   *
   * 2. co_direct_chan(pid):
   *    The process is suspended after a previous direct co_handoff().
   *    If we hand off directly to such a process, it resumes after
   *    co_handoff() inside sys_co_yield, so sys_co_yield must clear chan
   *    and release p->lock explicitly.
   *
   * Locking policy:
   * - wait_lock is used only to protect the rendezvous decision.
   * - Before the direct switch, wait_lock is always released.
   * - target->lock is kept held across the switch in both direct-handoff
   *   cases. This avoids a lock-free window before swtch(), and ensures
   *   that the resumed target continues with the lock state it expects:
   *     * sleep-wait target releases it inside sleep()
   *     * direct-wait target releases it after co_handoff() returns
   *
   * Deliberate limitation:
   * This implementation is intentionally limited to the assignment's
   * single-CPU cooperative scenario, without adding new fields to struct
   * proc, new process states, or new global kernel data structures.
   */

  // Case 1: target is sleeping inside sleep(..., &wait_lock).
  if(target->state == SLEEPING && target->chan == co_sleep_chan(target->pid)){
    acquire(&target->lock);

    // Re-check under target->lock.
    if(target->killed || target->state != SLEEPING ||
       target->chan != co_sleep_chan(target->pid)){
      release(&target->lock);
      release(&wait_lock);
      return -1;
    }

    // Current process will wait for the opposite yield in direct-wait mode.
    p->chan = co_direct_chan(p->pid);
    p->state = SLEEPING;

    // Deliver the value that becomes target's co_yield() return value.
    target->trapframe->a0 = value;
    target->state = RUNNING;

    // Important:
    // wait_lock must not be held across the direct switch.
    release(&wait_lock);

    // target->lock remains held across the switch on purpose.
    // The target will resume inside sleep(), and sleep() will release it.
    co_handoff(p, target);

    // We resume here when another process later yields back to us.
    // In that future handoff, our own lock is held across the switch.
    co_resume_cleanup(p);

    if(p->killed)
      return -1;

    return p->trapframe->a0;
  }
  // Case 2: target is suspended after a previous direct handoff.
  if(target->state == SLEEPING && target->chan == co_direct_chan(target->pid)){
    acquire(&target->lock);

    // Re-check under target->lock.
    if(target->killed || target->state != SLEEPING ||
       target->chan != co_direct_chan(target->pid)){
      release(&target->lock);
      release(&wait_lock);
      return -1;
    }

    // Current process will wait for the opposite yield in direct-wait mode.
    p->chan = co_direct_chan(p->pid);
    p->state = SLEEPING;

    // Deliver the value that becomes target's co_yield() return value.
    target->trapframe->a0 = value;
    target->state = RUNNING;

    // wait_lock must not be held across the direct switch.
    release(&wait_lock);

    // target->lock remains held across the switch here too.
    // In this case the target resumes after co_handoff(), so it will
    // release its own lock explicitly in sys_co_yield.
    co_handoff(p, target);

    // We resume here when another process later yields back to us.
    // Our own lock is held across that direct switch and must be released.
    co_resume_cleanup(p);

    if(p->killed)
      return -1;

    return p->trapframe->a0;
  }

  // Fallback: target is not ready yet.
  sleep(co_sleep_chan(p->pid), &wait_lock);
  release(&wait_lock);

  if(p->killed)
    return -1;

  return p->trapframe->a0;
}
