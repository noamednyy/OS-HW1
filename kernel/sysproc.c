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
co_chan(int pid)
{
  return (void*)(uint64)pid;
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

  // Find target process.
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
   * Direct handoff implementation for the assignment.
   *
   * Design choices:
   * - This implementation is intended for the required single-CPU setup.
   * - It supports the assignment's cooperative coroutine scenario, where
   *   two processes repeatedly yield to each other.
   * - If the target process is already blocked waiting for co_yield on its
   *   rendezvous channel, we pass the value directly and switch to it with
   *   swtch(), bypassing the regular scheduler.
   * - Otherwise, we use a fallback path in which the current process sleeps
   *   until another cooperating process yields to it.
   *
   * Deliberate limitation:
   * We do not attempt to fully resolve every possible race involving several
   * unrelated processes trying to yield to the same target concurrently.
   * This keeps the implementation compatible with the assignment's
   * restrictions: no new process fields, no new process states, and no new
   * global kernel data structures.
   *
   * Locking note:
   * The direct switch is done only when the target is already sleeping in
   * sleep(chan, &wait_lock). In that case, the target expects to resume with
   * target->lock still held, exactly as if it had been resumed by the normal
   * scheduler. Symmetrically, the current process switches out while holding
   * p->lock, and when another process later yields back to it, it resumes
   * with p->lock held and releases it before returning to userspace.
   */

  if(target->state == SLEEPING && target->chan == co_chan(target->pid)){
    acquire(&target->lock);

    // Re-check after taking target->lock.
    if(target->killed || target->state != SLEEPING ||
       target->chan != co_chan(target->pid)){
      release(&target->lock);
      release(&wait_lock);
      return -1;
    }

    // Current process will now wait for the opposite yield.
    acquire(&p->lock);
    p->chan = co_chan(p->pid);
    p->state = SLEEPING;

    // Deliver the value to the target.
    // Important: do NOT clear target->chan here.
    // The target will clear it itself when it continues after sleep().
    target->trapframe->a0 = value;
    target->state = RUNNING;

    release(&wait_lock);

    // Switch directly from p to target.
    co_handoff(p, target);

    // We resume here when another process yields back to us,
    // or if the scheduler runs us again after a wakeup/kill.
    p->chan = 0;
    release(&p->lock);

    if(p->killed)
      return -1;

    return p->trapframe->a0;
  }

  // Fallback path: target is not ready yet, so sleep until somebody yields to us.
  sleep(co_chan(p->pid), &wait_lock);
  release(&wait_lock);

  if(p->killed)
    return -1;

  return p->trapframe->a0;
}