#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
    int pid1 = getpid(); // Parent PID
    int pid2 = fork(); // Child PID
    if (pid2 == 0) { // Child
    for (;;) {
    int value = co_yield(pid1, 1);
    printf("Child received: %d\n", value); // Should print 2
    }
    } else { // Parent
    for (;;) {
    int value = co_yield(pid2, 2);
    printf("parent received: %d\n", value); // Should print 1
    }
    }
}