#include<stdlib.h>
#include <stdio.h>
#include<unistd.h>

int main(){
	int pid;
	pid = fork();
	if(pid == 0){
		printf("[Child]: Hello, pid=%d\n",getpid());
	}
	else{
		printf("[Parent] : Hello, pid=%d\n", getpid());
	}
}
