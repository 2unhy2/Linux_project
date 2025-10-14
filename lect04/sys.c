#include <sys/wait.h>
#include <stdio.h>

int main()
{
    int status;
    if ((status = system("date")) < 0)
        perror("system() error");
    printf("Return code= %d(%x), WEXITSTATUS(status): %d\n", 
            status, status, WEXITSTATUS(status));
    
    if ((status = system("hello")) < 0)
        perror("system() error");
    printf("Return code= %d(%x), WEXITSTATUS(status): %d\n", 
            status, status, WEXITSTATUS(status));
    
    if ((status = system("who; exit 44")) < 0)
        perror("system() error");
    printf("Return code= %d(%x), WEXITSTATUS(status): %d\n", 
            status, status, WEXITSTATUS(status));

    return 0;
}
