# コアのテストを PC で動かす: make -C lib/mas/test
CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../include
SRC = test_core.c ../src/can/core/md.c ../src/can/core/setting.c

test: test_core
	./test_core

test_core: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f test_core

.PHONY: test clean
