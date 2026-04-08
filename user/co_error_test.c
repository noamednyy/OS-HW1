#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int ret;
  int pid = getpid();
  int child;

  printf("== co_yield error tests ==\n");

  // 1. Non-existent PID
  ret = co_yield(99999, 1);
  printf("yield to non-existent pid: %d\n", ret);

  // 2. Self yield
  ret = co_yield(pid, 1);
  printf("yield to self: %d\n", ret);

  // 3. Yield to killed process
  child = fork();
  if(child < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(child == 0){
    // child exits immediately
    exit(0);
  }

  wait(0);  // child is now dead
  ret = co_yield(child, 1);
  printf("yield to killed process: %d\n", ret);

  exit(0);
}