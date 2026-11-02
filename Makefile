# Build: make   (expects ort/include and ort/lib from ./fetch.sh)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
ORT     ?= ort

gigaam: gigaam.c
	$(CC) $(CFLAGS) -I$(ORT)/include gigaam.c -o gigaam \
	    -L$(ORT)/lib -lonnxruntime -Wl,-rpath,'$$ORIGIN/$(ORT)/lib' -lm

clean:
	rm -f gigaam
