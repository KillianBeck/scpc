CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  CFLAGS += -D_DARWIN_C_SOURCE
  LDFLAGS += -pthread -lm
else
  LDFLAGS += -pthread -lm -lutil
endif

LIB_SRCS = src/cli.c src/scanner.c src/progress.c src/workers.c src/runner.c
BIN      = scpc
TEST_BIN = test_scpc

.PHONY: all test clean install

all: $(BIN)

$(BIN): src/main.c $(LIB_SRCS) src/scpc.h
	$(CC) $(CFLAGS) -o $(BIN) src/main.c $(LIB_SRCS) $(LDFLAGS)

$(TEST_BIN): tests/test_scpc.c $(LIB_SRCS) src/scpc.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) tests/test_scpc.c $(LIB_SRCS) $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

install: $(BIN)
	install -d /usr/local/bin
	install -m 755 $(BIN) /usr/local/bin/$(BIN)

clean:
	rm -f $(BIN) $(TEST_BIN)

