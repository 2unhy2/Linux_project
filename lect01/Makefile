exe: main.o copy.o
	gcc -o exe main.o copy.o
main.o: main.c
	gcc -c main.c -I./include
copy.o: copy.c
	gcc -c copy.c -I./include
