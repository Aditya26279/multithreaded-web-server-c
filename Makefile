CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  :=

ifeq ($(OS),Windows_NT)
  EXE    := .exe
  LDLIBS += -lws2_32
else
  EXE    :=
  CFLAGS += -D_POSIX_C_SOURCE=200809L -pthread
  LDLIBS += -pthread
endif

COMMON := src/common.c src/http.c src/log.c src/queue.c

all: bin/server_mt$(EXE) bin/server_st$(EXE) bin/loadtest$(EXE)

bin:
	mkdir -p bin

bin/server_mt$(EXE): src/server_mt.c src/threadpool.c $(COMMON) src/*.h | bin
	$(CC) $(CFLAGS) src/server_mt.c src/threadpool.c $(COMMON) -o $@ $(LDLIBS)

bin/server_st$(EXE): src/server_st.c $(COMMON) src/*.h | bin
	$(CC) $(CFLAGS) src/server_st.c $(COMMON) -o $@ $(LDLIBS)

bin/loadtest$(EXE): src/loadtest.c src/compat.h | bin
	$(CC) $(CFLAGS) src/loadtest.c -o $@ $(LDLIBS)

bench: all
	python3 benchmark.py

clean:
	rm -rf bin

.PHONY: all bench clean
