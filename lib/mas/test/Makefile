# テストを PC で動かす: make -C lib/mas/test
CC ?= gcc
CXX ?= g++
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../include
CXXFLAGS = -std=c++11 -Wall -Wextra -Werror -I../include
CORE = ../src/can/core/md.c ../src/can/core/setting.c
LIB = ../src/can/bus.cpp ../src/can/md.cpp ../src/can/controller.cpp

test: test_core test_bus
	./test_core
	./test_bus

test_core: test_core.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

test_bus: test_bus.cpp $(LIB) $(CORE)
	$(CC) $(CFLAGS) -c $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ test_bus.cpp $(LIB) md.o setting.o

clean:
	rm -f test_core test_bus *.o

.PHONY: test clean
