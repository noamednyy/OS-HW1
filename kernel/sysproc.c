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
   * Key idea:
   * We distinguish between two waiting modes using two different channels:
   *
   * 1. co_sleep_chan(pid):
   *    The process is blocked inside sleep(..., &wait_lock).
   *    In this case, it is correct to resume it while still holding
   *    target->lock, because the resumed process continues inside sleep()
   *    and releases that lock there according to the normal xv6 protocol.
   *
   * 2. co_direct_chan(pid):
   *    The process is suspended after a previous direct co_handoff().
   *    In this case, it must NOT be resumed with target->lock still held,
   *    because it does not continue from sleep() and therefore would not
   *    release that lock itself.
   *
   * This keeps the code simple and compatible with the assignment:
   * no new process fields, no new process states, and no new global
   * kernel data structures.
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

    // Current process waits for the opposite yield in direct-wait mode.
    p->chan = co_direct_chan(p->pid);
    p->state = SLEEPING;

    // Deliver the value that becomes target's co_yield() return value.
    target->trapframe->a0 = value;
    target->state = RUNNING;

    // Important:
    // We do NOT clear target->chan here.
    // The resumed target will continue inside sleep(), clear chan itself,
    // release target->lock there, and reacquire wait_lock as usual.
    release(&wait_lock);

    // Direct handoff to a process that resumes from sleep().
    // target->lock stays held across the switch on purpose.
    co_handoff(p, target);

    // We resume here when another process later yields back to us.
    p->chan = 0;

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

    // Current process now waits for the opposite yield in direct-wait mode.
    p->chan = co_direct_chan(p->pid);
    p->state = SLEEPING;

    // Deliver the value that becomes target's co_yield() return value.
    target->trapframe->a0 = value;
    target->state = RUNNING;

    // Important:
    // Here target resumes AFTER co_handoff(), not from sleep().
    // Therefore target->lock must be released before the switch,
    // otherwise it would remain held and cause panic: acquire later.
    release(&target->lock);
    release(&wait_lock);

    // Direct handoff to a process that resumes after co_handoff().
    co_handoff(p, target);

    // We resume here when another process later yields back to us.
    p->chan = 0;

    if(p->killed)
      return -1;

    return p->trapframe->a0;
  }

  // Fallback: target is not ready yet.
  // Sleep using the regular sleep protocol.
  sleep(co_sleep_chan(p->pid), &wait_lock);
  release(&wait_lock);

  if(p->killed)
    return -1;

  return p->trapframe->a0;
}