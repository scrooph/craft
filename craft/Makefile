#makefile for craft
CC=gcc
BINDIR = $(prefix)/usr/sbin
MANDIR = $(prefix)/usr/share/man

all: craft
craft: craft.o sink.o transfer.o backup.o
	${CC} -o craft craft.o sink.o transfer.o backup.o ${LIBS} -pthread -lsqlite3
sink.o: sink.c
	${CC} -c sink.c
craft.o: craft.c
	${CC} -c craft.c -pthread
transfer.o:transfer.c
	${CC} -c transfer.c -lsqlite3
backup.o:backup.c
	${CC} -c backup.c
clean:
	rm -f *.o
