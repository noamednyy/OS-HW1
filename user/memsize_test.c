#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  char *p;

  printf("Memory before malloc: %d bytes\n", memsize());

  p = malloc(20000);
  if(p == 0){
    printf("malloc failed\n");
    exit(1);
  }

  printf("Memory after malloc: %d bytes\n", memsize());

  free(p);

  printf("Memory after free: %d bytes\n", memsize());

  exit(0);
}