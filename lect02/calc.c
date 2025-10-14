#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]){
	int num1, num2, result;
	char operator;

	num1 = atoi(argv[1]);//1부터 시작하니까
	num2 = atoi(argv[3]);

	operator = argv[2][0];

	switch(operator){
		case '+':
			result = num1 + num2;
			break;
		case '-':
			result = num1 - num2;
			break;
		case 'X':
			result = num1 * num2;
			break;
		case '/':
			result = num1/num2;
			break;
	}
	printf("%d\n", result);
	return 0;
}
