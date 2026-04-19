// ttest - basic smoke tests for the thread API.
// See Threads.md for the API contract being exercised here.

#include "types.h"
#include "stat.h"
#include "user.h"

#define STACK_SIZE 4096

static volatile int counter;
static char stacks[4][STACK_SIZE] __attribute__((aligned(16)));

static void *
counting_worker(void)
{
  int i;
  for(i = 0; i < 100; i++)
    counter++;
  thread_exit((void*)0x1234);
}

static void
test_create_join(void)
{
  tid_t t1, t2;
  void *ev1 = 0, *ev2 = 0;

  printf(1, "test_create_join... ");
  counter = 0;
  if(thread_create(&t1, counting_worker, stacks[0], STACK_SIZE) < 0){
    printf(1, "FAIL create1\n"); return;
  }
  if(thread_create(&t2, counting_worker, stacks[1], STACK_SIZE) < 0){
    printf(1, "FAIL create2\n"); return;
  }
  if(thread_join(t1, &ev1) < 0){ printf(1, "FAIL join1\n"); return; }
  if(thread_join(t2, &ev2) < 0){ printf(1, "FAIL join2\n"); return; }
  if(counter != 200){
    printf(1, "FAIL counter=%d (want 200)\n", counter); return;
  }
  if((int)ev1 != 0x1234 || (int)ev2 != 0x1234){
    printf(1, "FAIL exit values %p %p\n", ev1, ev2); return;
  }
  printf(1, "ok (counter=%d)\n", counter);
}

static int shared_pid;

static void *
id_reporter(void)
{
  uint tid = thread_getThreadId();
  uint pid = thread_getProcessId();
  if((int)pid != shared_pid)
    printf(1, "  MISMATCH tid=%d pid=%d (expected pid %d)\n",
           tid, pid, shared_pid);
  thread_exit((void*)tid);
}

static void
test_ids(void)
{
  tid_t t;
  void *ev = 0;

  printf(1, "test_ids... ");
  shared_pid = getpid();
  if(thread_create(&t, id_reporter, stacks[0], STACK_SIZE) < 0){
    printf(1, "FAIL create\n"); return;
  }
  if(thread_join(t, &ev) < 0){ printf(1, "FAIL join\n"); return; }
  if((uint)ev != (uint)t){
    printf(1, "FAIL ev=%p tid=%d\n", ev, t); return;
  }
  if((uint)t == (uint)shared_pid){
    printf(1, "FAIL tid == pid (thread should have distinct tid)\n"); return;
  }
  printf(1, "ok (pid=%d tid=%d)\n", shared_pid, t);
}

static void *
long_liver(void)
{
  int i;
  for(i = 0; i < 50; i++)
    counter++;
  thread_exit((void*)0xBEEF);
}

static void
test_main_thread_exit(void)
{
  // If the main thread calls thread_exit while another thread is alive,
  // the process must keep running; the parent's wait() should only
  // unblock when the last thread in the group exits.
  int pid;
  printf(1, "test_main_thread_exit... ");
  counter = 0;
  if((pid = fork()) < 0){ printf(1, "FAIL fork\n"); return; }
  if(pid == 0){
    tid_t t;
    if(thread_create(&t, long_liver, stacks[0], STACK_SIZE) < 0){
      printf(1, "FAIL child create\n");
      exit();
    }
    thread_exit((void*)0);  // main exits as a thread; long_liver continues
  }
  if(wait() != pid){ printf(1, "FAIL wait\n"); return; }
  printf(1, "ok\n");
}

static void *
process_killer(void)
{
  exit();  // kills the entire process
}

static void
test_exit_from_thread(void)
{
  int pid;
  printf(1, "test_exit_from_thread... ");
  if((pid = fork()) < 0){ printf(1, "FAIL fork\n"); return; }
  if(pid == 0){
    tid_t t;
    if(thread_create(&t, process_killer, stacks[0], STACK_SIZE) < 0){
      printf(1, "FAIL child create\n");
      exit();
    }
    while(1)        // wait to be killed; trap return observes killed=1
      ;
  }
  if(wait() != pid){ printf(1, "FAIL wait\n"); return; }
  printf(1, "ok\n");
}

int
main(int argc, char *argv[])
{
  printf(1, "ttest: xv6 thread smoke tests\n");
  test_create_join();
  test_ids();
  test_main_thread_exit();
  test_exit_from_thread();
  printf(1, "ttest: done\n");
  exit();
}
