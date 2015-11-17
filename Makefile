CFLAGS=-g
LFLAGS=-g -lpthread

XENO_CFLAGS=-g -D__XENO__ $(shell xeno-config --skin=posix --cflags)
XENO_LFLAGS=-g $(shell xeno-config --skin=posix --ldflags)

.phony: all

all: run rtrun

run: main.o
	gcc -o $@ $^ $(LFLAGS)

main.o: main.c
	gcc -c -o $@ $< $(CFLAGS)


rtrun: main.rt.o
	gcc -o $@ $^ $(XENO_LFLAGS)

main.rt.o: main.c
	gcc -c -o $@ $< $(XENO_CFLAGS)
