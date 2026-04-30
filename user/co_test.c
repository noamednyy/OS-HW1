#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
    int pid1 = getpid(); // Parent PID

    printf("--- Running Error Tests ---\n");
    
    // Test (c): Self-yield
    int ret = co_yield(pid1, 1);
    if(ret == -1) printf("Test (c) passed: self-yield failed.\n");
    else printf("Test (c) FAILED.\n");

    // Test (a): Non-existent PID
    ret = co_yield(9999, 1);
    if(ret == -1) printf("Test (a) passed: non-existent PID failed.\n");
    else printf("Test (a) FAILED.\n");

    // Test (b): Killed process
    int kill_pid = fork();
    if(kill_pid == 0) {
        for(;;){
           sleep(10);
        }
    } else {
        kill(kill_pid);
        // Sleep to allow the child to be killed
        sleep(10); 
        ret = co_yield(kill_pid, 1);
        if(ret == -1) printf("Test (b) passed: yield to killed process failed.\n");
        else printf("Test (b) FAILED.\n");
        wait(0);
    }

    printf("\n--- Starting Ping-Pong ---\n");
    int pid2 = fork(); // Child PID
    if (pid2 == 0) { // Child
        for (int i=0; i<300; i++) {
            int value = co_yield(pid1, 1);
            printf("Child received: %d\n", value); // Should print 2
        }
    } else { // Parent
        for (int i=0; i<300; i++) {
            int value = co_yield(pid2, 2);
            printf("parent received: %d\n", value); // Should print 1
        }
        wait(0);
    }
    
    exit(0);
}