#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  // Threads: getpid() reports the process id (leader's pid), not the
  // caller's thread id. Use thread_getThreadId() for the thread id.
  return thread_leader(myproc())->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Thread syscalls (see Threads.md and proc.c).

int
sys_thread_create(void)
{
  char *tid_ptr, *entry, *stack;
  int stack_size;

  if(argptr(0, &tid_ptr, sizeof(uint)) < 0)
    return -1;
  if(argint(1, (int*)&entry) < 0)
    return -1;
  if(argint(2, (int*)&stack) < 0)
    return -1;
  if(argint(3, &stack_size) < 0)
    return -1;
  if(stack_size <= 0)
    return -1;
  // argptr already bounds-checks tid_ptr against curproc->sz, but the
  // caller-supplied stack is only bounds-checked indirectly via copyout
  // inside thread_create (walkpgdir validates the mapping).
  return thread_create((uint*)tid_ptr, (void*)entry, (void*)stack,
                       (uint)stack_size);
}

int
sys_thread_exit(void)
{
  int ev;
  // exit_value is an opaque pointer-sized value from the user; we do
  // not dereference it, so argint suffices.
  if(argint(0, &ev) < 0)
    return -1;
  thread_exit((void*)ev);
  return 0;  // unreachable
}

int
sys_thread_join(void)
{
  int tid;
  char *ev_ptr;

  if(argint(0, &tid) < 0)
    return -1;
  if(argptr(1, &ev_ptr, sizeof(void*)) < 0)
    return -1;
  return thread_join((uint)tid, (void**)ev_ptr);
}

int
sys_thread_getThreadId(void)
{
  return (int)thread_getThreadId();
}

int
sys_thread_getProcessId(void)
{
  return (int)thread_getProcessId();
}
