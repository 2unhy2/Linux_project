#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* 자식 프로세스를 생성하여 echo 명령어를 실행한다. */
int main()
{
    if (fork() == 0) {
        char *arg[3];
        arg[0] = "echo"; arg[1] = "hello"; arg[2] = NULL;
        execv("/bin/echo", arg);
    }
    printf("End of parent process\n");
    return 0;
}
