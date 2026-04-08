#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int pid1 = getpid();
  int pid2 = fork();
  int i;

  if(pid2 < 0){
    printf("fork failed\n");
    exit(1);
  }

  if(pid2 == 0){
    for(i = 0; i < 5; i++){
      int value = co_yield(pid1, 1);
      if(value < 0){
        printf("child: co_yield failed\n");
        exit(1);
      }
      printf("Child received: %d\n", value);
    }
    exit(0);
  } else {
    for(i = 0; i < 5; i++){
      int value = co_yield(pid2, 2);
      if(value < 0){
        printf("parent: co_yield failed\n");
        kill(pid2);
        wait(0);
        exit(1);
      }
      printf("Parent received: %d\n", value);
    }
    wait(0);
    exit(0);
  }
}