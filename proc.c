#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// Return the leader (group-owning proc) for p. For a non-thread p this is p
// itself; for a thread slot it is p->thread_group. Uses pointer identity, so
// safe to call on any live struct proc.
struct proc *
thread_leader(struct proc *p)
{
  return p->is_thread ? p->thread_group : p;
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  // Thread defaults: fresh slots represent single-threaded processes.
  p->is_thread = 0;
  p->thread_group = 0;
  p->thread_count = 1;
  p->thread_exit_value = 0;
  p->thread_exited = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow the current process's memory by n bytes.
// Return 0 on success, -1 on failure.
//
// Thread semantics: pgdir is shared across all threads. We serialize
// the grow under ptable.lock and propagate the new sz to every thread
// in the group so their argptr/argint bounds checks stay consistent.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *p;

  acquire(&ptable.lock);
  sz = leader->sz;
  if(n > 0){
    if((sz = allocuvm(leader->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  } else if(n < 0){
    if((sz = deallocuvm(leader->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
  }
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED) continue;
    if(thread_leader(p) == leader)
      p->sz = sz;
  }
  release(&ptable.lock);
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(leader->pgdir, leader->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = leader->sz;
  // Parent is the enclosing process's leader, not the calling thread:
  // the child's lifetime is tied to the process, not to a single thread.
  np->parent = leader;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  // The fd table and cwd are shared group-wide and live on the leader.
  // Dup from there so the child inherits what the process (not the
  // calling thread) currently has open.
  for(i = 0; i < NOFILE; i++)
    if(leader->ofile[i])
      np->ofile[i] = filedup(leader->ofile[i]);
  np->cwd = idup(leader->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process (whole process, including all threads).
// An exited thread remains ZOMBIE until joined (thread_join) or reaped
// by the parent via wait() along with its group leader.
//
// Thread semantics: any thread calling exit() terminates the entire
// process. Siblings are asked to die (killed=1, SLEEPING→RUNNABLE);
// they finish their syscall, trap out, and hit this same function.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *p;
  int fd, is_last;

  if(curproc == initproc)
    panic("init exiting");

  acquire(&ptable.lock);

  // Ask all other members of the group to die. They will observe
  // killed=1 on their next trap return and re-enter exit() themselves.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p == curproc) continue;
    if(p->state == UNUSED) continue;
    if(thread_leader(p) == leader){
      p->killed = 1;
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
    }
  }

  // Reparent the leader's children. Idempotent across multiple exiters.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == leader){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  leader->thread_count--;
  is_last = (leader->thread_count == 0);

  release(&ptable.lock);

  // Last-thread-out closes the shared fd table and releases the cwd.
  // Earlier exiters leave them intact so surviving threads (which will
  // reach exit() on their own trap return) can keep using them. The
  // fileclose/iput calls may sleep, so we drop ptable.lock first.
  if(is_last){
    for(fd = 0; fd < NOFILE; fd++){
      if(leader->ofile[fd]){
        fileclose(leader->ofile[fd]);
        leader->ofile[fd] = 0;
      }
    }
    begin_op();
    iput(leader->cwd);
    end_op();
    leader->cwd = 0;
  }

  acquire(&ptable.lock);

  wakeup1(curproc);          // any thread_join on us
  wakeup1(leader);           // any exec/thread_exit waiter on the group
  wakeup1(leader->parent);   // parent's wait() — cheap to call repeatedly

  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process (not a thread) to exit and return its pid.
// Return -1 if this process has no children.
//
// Thread semantics: wait() reaps at the process level. It only unblocks
// when a child's entire thread group is ZOMBIE (so it is safe to free
// the shared pgdir). Thread slots within the same process are never
// visible as children to wait() — use thread_join for them.
int
wait(void)
{
  struct proc *p, *q;
  int havekids, pid;
  struct proc *curproc = myproc();
  pde_t *to_free;

  acquire(&ptable.lock);
  for(;;){
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      if(p->is_thread)              // threads are reaped by thread_join
        continue;
      havekids = 1;
      if(p->state != ZOMBIE)
        continue;

      // Leader is ZOMBIE. Only reap if every group member is ZOMBIE —
      // otherwise sibling threads may still be executing with this pgdir.
      int all_zombie = 1;
      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
        if(q == p) continue;
        if(q->state == UNUSED) continue;
        if(thread_leader(q) == p && q->state != ZOMBIE){
          all_zombie = 0;
          break;
        }
      }
      if(!all_zombie)
        continue;

      pid = p->pid;
      to_free = p->pgdir;

      // Reap every slot belonging to the group (leader + threads).
      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
        if(q->state == UNUSED) continue;
        if(thread_leader(q) != p) continue;
        kfree(q->kstack);
        q->kstack = 0;
        q->pid = 0;
        q->parent = 0;
        q->name[0] = 0;
        q->killed = 0;
        q->is_thread = 0;
        q->thread_group = 0;
        q->thread_count = 0;
        q->thread_exit_value = 0;
        q->thread_exited = 0;
        q->pgdir = 0;
        q->sz = 0;
        q->state = UNUSED;
      }
      freevm(to_free);
      release(&ptable.lock);
      return pid;
    }

    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process containing the thread/process with the given pid.
// All threads in the group are asked to exit. They won't actually exit
// until each returns to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p, *q, *leader;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED) continue;
    if(p->pid != pid) continue;

    leader = thread_leader(p);
    for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
      if(q->state == UNUSED) continue;
      if(thread_leader(q) != leader) continue;
      q->killed = 1;
      if(q->state == SLEEPING)
        q->state = RUNNABLE;
    }
    release(&ptable.lock);
    return 0;
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 32
// Thread primitives (see Threads.md).
//
// A thread shares its creator's pgdir and sz and the group-wide fd
// table and cwd (which live on the leader), but owns its own kstack,
// trap frame, and scheduling context.

// thread_create: spawn a sibling thread in the same group as curproc.
// entry, stack are user addresses; the new thread begins at entry with
// %esp pointing one word below stack+stack_size where we have stashed
// a bogus return PC (0xffffffff) so that a bare "ret" from entry faults
// clearly instead of silently falling off the stack.
// Returns 0 and writes the new tid to *out_tid on success; -1 on error.
int
thread_create(uint *out_tid, void *entry, void *stack, uint stack_size)
{
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *nt;
  uint sp;
  uint fake_ret = 0xffffffff;

  if(entry == 0 || stack == 0 || stack_size < 4)
    return -1;

  if((nt = allocproc()) == 0)
    return -1;

  // Share address space with the group. Files and cwd are reached via
  // thread_leader(nt) from sysfile.c / fs.c, so non-leader thread slots
  // intentionally leave their ofile[] empty and cwd NULL.
  nt->pgdir = leader->pgdir;
  nt->sz    = leader->sz;
  nt->parent = leader->parent;
  *nt->tf = *curproc->tf;

  safestrcpy(nt->name, leader->name, sizeof(nt->name));

  // Plant the fake return address and arrange user entry state.
  sp = (uint)stack + stack_size - 4;
  if(copyout(nt->pgdir, sp, &fake_ret, sizeof(fake_ret)) < 0)
    goto fail;
  nt->tf->eip = (uint)entry;
  nt->tf->esp = sp;
  nt->tf->eax = 0;

  // Hand the tid back to the caller if they asked for it.
  if(out_tid){
    if(copyout(leader->pgdir, (uint)out_tid, &nt->pid, sizeof(int)) < 0)
      goto fail;
  }

  acquire(&ptable.lock);
  nt->is_thread = 1;
  nt->thread_group = leader;
  nt->thread_count = 0;  // unused on non-leader slots
  leader->thread_count++;
  nt->state = RUNNABLE;
  release(&ptable.lock);

  return 0;

fail:
  kfree(nt->kstack);
  nt->kstack = 0;
  acquire(&ptable.lock);
  nt->pid = 0;
  nt->state = UNUSED;
  release(&ptable.lock);
  return -1;
}

// thread_exit: terminate the calling thread, publishing exit_value for
// any thread_join waiter. If this is the last thread in the group, the
// parent's wait() unblocks (just like a whole-process exit). This is a
// thread-scoped termination; it never kills siblings — that is exit().
void
thread_exit(void *exit_value)
{
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *p;
  int fd, is_last;

  if(curproc == initproc)
    panic("init thread_exit");

  acquire(&ptable.lock);

  curproc->thread_exit_value = exit_value;
  curproc->thread_exited = 1;
  leader->thread_count--;
  is_last = (leader->thread_count == 0);

  if(is_last){
    // Adopt-out any children the leader still has.
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == leader){
        p->parent = initproc;
        if(p->state == ZOMBIE)
          wakeup1(initproc);
      }
    }
  }

  release(&ptable.lock);

  // Last-thread-out releases the shared fd table and cwd. Others leave
  // them intact for surviving threads to continue using.
  if(is_last){
    for(fd = 0; fd < NOFILE; fd++){
      if(leader->ofile[fd]){
        fileclose(leader->ofile[fd]);
        leader->ofile[fd] = 0;
      }
    }
    begin_op();
    iput(leader->cwd);
    end_op();
    leader->cwd = 0;
  }

  acquire(&ptable.lock);

  wakeup1(curproc);          // joiners on this thread
  wakeup1(leader);           // exec/exit waiters on the group
  if(is_last)
    wakeup1(leader->parent); // parent's wait() — only unblocks on last

  curproc->state = ZOMBIE;
  sched();
  panic("zombie thread_exit");
}

// thread_join: block until the target thread (must belong to the same
// group) has exited. On success returns 0 and, if exit_value_ptr is
// non-NULL, stores the thread's exit value there.
//
// Reaping: for a non-leader thread slot we free its kstack and return
// the slot to UNUSED here. A leader's slot is not reaped here — the
// parent's wait() owns that to keep the process-level lifecycle intact.
int
thread_join(uint tid, void **exit_value_ptr)
{
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *target;
  void *ev;

  if(tid == 0 || tid == (uint)curproc->pid)
    return -1;

  acquire(&ptable.lock);
  for(target = ptable.proc; target < &ptable.proc[NPROC]; target++){
    if(target->state != UNUSED && (uint)target->pid == tid)
      break;
  }
  if(target == &ptable.proc[NPROC] || thread_leader(target) != leader){
    release(&ptable.lock);
    return -1;
  }

  while(target->state != ZOMBIE){
    if(curproc->killed){
      release(&ptable.lock);
      return -1;
    }
    sleep(target, &ptable.lock);
    // After wakeup, confirm the slot is still our target — defensive
    // against theoretical reuse, though the lock prevents it in practice.
    if((uint)target->pid != tid){
      release(&ptable.lock);
      return -1;
    }
  }

  ev = target->thread_exit_value;

  if(target->is_thread){
    kfree(target->kstack);
    target->kstack = 0;
    target->pid = 0;
    target->parent = 0;
    target->name[0] = 0;
    target->killed = 0;
    target->is_thread = 0;
    target->thread_group = 0;
    target->thread_count = 0;
    target->thread_exit_value = 0;
    target->thread_exited = 0;
    target->pgdir = 0;
    target->sz = 0;
    target->state = UNUSED;
  }
  release(&ptable.lock);

  if(exit_value_ptr){
    // exit_value_ptr lives in our address space (validated by argptr).
    *exit_value_ptr = ev;
  }
  return 0;
}

uint
thread_getThreadId(void)
{
  return (uint)myproc()->pid;
}

uint
thread_getProcessId(void)
{
  return (uint)thread_leader(myproc())->pid;
}

// drain_thread_group: called before an image swap to flatten a
// multi-threaded process into a single-threaded one. Kills all sibling
// threads in the group and reaps them in place so that when the caller
// swaps pgdir and frees the old one no thread is left referring to it.
// On return curproc is the sole member of a fresh single-threaded group.
//
// Re-kills on each iteration: a sibling whose syscall was in flight
// when we first ran may have spawned a new thread before observing
// killed=1, so we keep sweeping until every non-self slot in the group
// is ZOMBIE or UNUSED.
void
drain_thread_group(void)
{
  struct proc *curproc = myproc();
  struct proc *leader = thread_leader(curproc);
  struct proc *p;
  int fd;

  acquire(&ptable.lock);

  for(;;){
    int alive = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p == curproc) continue;
      if(p->state == UNUSED) continue;
      if(thread_leader(p) != leader) continue;
      if(p->state == ZOMBIE) continue;
      p->killed = 1;
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      alive++;
    }
    if(alive == 0) break;
    sleep(leader, &ptable.lock);
  }

  // If curproc is a non-leader thread, the shared fd table and cwd
  // live on the leader's slot — which we are about to reap. Move
  // them onto curproc, which becomes the new leader below. A pure
  // pointer transfer; no refcount changes, no sleeping calls.
  if(curproc != leader){
    for(fd = 0; fd < NOFILE; fd++){
      curproc->ofile[fd] = leader->ofile[fd];
      leader->ofile[fd] = 0;
    }
    curproc->cwd = leader->cwd;
    leader->cwd = 0;
  }

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p == curproc) continue;
    if(p->state != ZOMBIE) continue;
    if(thread_leader(p) != leader) continue;
    kfree(p->kstack);
    p->kstack = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->killed = 0;
    p->is_thread = 0;
    p->thread_group = 0;
    p->thread_count = 0;
    p->thread_exit_value = 0;
    p->thread_exited = 0;
    p->pgdir = 0;
    p->sz = 0;
    p->state = UNUSED;
  }

  curproc->is_thread = 0;
  curproc->thread_group = 0;
  curproc->thread_count = 1;
  curproc->thread_exit_value = 0;
  curproc->thread_exited = 0;

  release(&ptable.lock);
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
