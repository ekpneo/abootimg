
CC=cc
#CFLAGS=-O3 -Wall -DHAS_BLKID
CFLAGS=-Wall -g -ggdb -DHAS_BLKID
LIBS= -lblkid

all: abootimg.o sha.o
	$(CC) $(LDLAGS) -o abootimg abootimg.o sha.o $(LIBS)

version.h:
	if [ ! -f version.h ]; then \
	if [ -d .git ]; then \
	echo '#define VERSION_STR "$(shell git describe --tags --abbrev=0)"' > version.h; \
	else \
	echo '#define VERSION_STR ""' > version.h; \
	fi \
	fi

abootimg.o: bootimg.h version.h
	$(CC) $(CFLAGS) -c -o abootimg.o abootimg.c

sha.o:
	$(CC) $(CFLAGS) -c -o sha.o minicript/sha.c

clean:
	rm -f abootimg *.o version.h

.PHONY:	clean all

