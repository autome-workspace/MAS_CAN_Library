# テストを PC で動かす: make -C lib/mas/test
CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../include
CORE = ../src/can/core/md.c ../src/can/core/setting.c

test: test_core test_node
	./test_core
	./test_node

test_core: test_core.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

test_node: test_node.c ../src/can/node.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_node.c ../src/can/node.c $(CORE)

clean:
	rm -f test_core test_bus test_node *.o

.PHONY: test clean
