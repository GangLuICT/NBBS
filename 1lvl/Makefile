CC=gcc
FLAGS=-g -Wall -lm
#INCLUDE= -I include/ -I mm/ -I core/ -Istatistics/
EXECUTABLE=test


ifdef MIN
FLAGS:= $(FLAGS) -DMIN_ALLOCABLE_BYTES=$(MIN)
endif

ifdef MAX
FLAGS:= $(FLAGS) -DMAX_ALLOCABLE_BYTE=$(MAX)
endif

all: test

corr: correctness.o nballoc.o utils.o
	$(CC) $(FLAGS) nballoc.o correctness.o utils.o -o corr
	
test: main.o nballoc.o utils.o
	$(CC) $(FLAGS) nballoc.o main.o utils.o -o $(EXECUTABLE)
	
main.o : main.c
	$(CC) $(FLAGS) main.c -o main.o -c

correctness.o : correctness.c
	$(CC) $(FLAGS) correctness.c -o correctness.o -c

register.o : nballoc.c nballoc.h timer.h	
	$(CC) $(FLAGS) nballoc.c -o nballoc.o -c
	
utils.o : utils.c utils.h
	$(CC) $(FLAGS) utils.c -o utils.o -c
	
clean:
	rm *.o test corr
