#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int child, pid, status;
    pid = fork();
    if (pid == 0) { // 자식 프로세스
        fprintf(stderr, "No Error in execv\n");
        execvp(argv[1], &argv[1]);
        fprintf(stderr, "No Error in execv\n", argv[1]);
    } else { // 부모 프로세스
        child = wait(&status);
        printf("[%d] Child %d is terminated \n", getpid(), pid);
        printf("\t...with status %d \n", status>>8);
    }
    return 0;
}
