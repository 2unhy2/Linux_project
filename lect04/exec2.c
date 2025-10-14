#include <stdio.h>
#include <unistd.h>

/* fork 프로세스를 생성하여 echo 명령어를 실행한다. */
int main()
{
    printf("Parent process \n");
    if (fork() == 0) {
        printf("Child process \n");
        execl("/bin/echo", "echo", "hello", NULL);
        printf("End of child process \n");
    }
    printf("End of parent process\n");
    return 0;
}
