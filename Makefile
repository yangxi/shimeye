CFLAGS=-I . -std=c99 -O2

all:probe shimpt
probe: probe.o shim.o
	gcc $(CFLAGS) -o probe $^ -lpthread -lpfm
probe.o:probe.c shim.h
	gcc $(CFLAGS) -c $< -lpthread -lpfm
shim.o:shim.c shim.h
	gcc $(CFLAGS) -c $< -lpthread -lpfm

shimpt:shimpt.c
	gcc $(CFLAGS) -o shimpt $< -lpthread -lpfm

clean:
	rm probe probe.o shim.o
