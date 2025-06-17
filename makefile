CC = gcc
CFLAGS = -Wall -pthread

all: MWCp

hw1: MWCp.c
	$(CC) $(CFLAGS) MWCp.c -o -lpthread MWCp

clean:
	rm -f MWCp
