#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

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
  if(n < 0)
    n = 0;
  backtrace(); // dump call stack for the traps lab's bttest diagnostic
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

// Configure a handler that runs every 'ticks' timer interrupts.
uint64
sys_sigalarm(void)
{
  int ticks;
  uint64 handler;

  argint(0, &ticks);
  argaddr(1, &handler);
  if(ticks < 0)
    ticks = 0; // negative intervals are treated as disabling the alarm

  struct proc *p = myproc();
  acquire(&p->lock);
  p->alarm_interval = ticks;
  p->alarm_handler = handler;
  if(ticks > 0){
    p->alarm_ticks_left = ticks;
  } else {
    p->alarm_ticks_left = 0;
    p->alarm_active = 0;
  }
  release(&p->lock);
  return 0;
}

// Return from the user alarm handler by restoring the saved context.
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  uint64 retval = 0;

  acquire(&p->lock);
  if(p->alarm_active){
    retval = p->alarm_trapframe.a0; // return whatever a0 held when the timer interrupt fired
    *(p->trapframe) = p->alarm_trapframe;
    p->alarm_active = 0;
    if(p->alarm_interval > 0)
      p->alarm_ticks_left = p->alarm_interval;
  }
  release(&p->lock);
  return retval;
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
