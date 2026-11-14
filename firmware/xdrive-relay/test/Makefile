# PC のテスト: make -C test（lib/mas はこのリポジトリのものを使う）
MAS ?= ../../..
LIB = $(MAS)/lib/mas
CC ?= gcc
CXX ?= g++
FLAGS = -Wall -Wextra -Werror -I../src -I$(LIB)/include
CORE = $(LIB)/src/can/node.c $(LIB)/src/can/core/md_frame.c $(LIB)/src/can/core/setting.c

test: test_relay
	./test_relay

test_relay: test_relay.cpp ../src/relay.cpp ../src/relay.h ../src/xd.h $(CORE)
	$(CC) -std=c11 $(FLAGS) -c $(CORE)
	$(CXX) -std=c++17 $(FLAGS) -o $@ test_relay.cpp ../src/relay.cpp node.o md_frame.o setting.o

clean:
	rm -f test_relay *.o

.PHONY: test clean
