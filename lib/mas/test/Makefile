# テストを PC で動かす: make -C lib/mas/test
CC ?= gcc
CXX ?= g++
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../include
CXXFLAGS = -std=c++11 -Wall -Wextra -Werror -I../include
CORE = ../src/can/core/md.c ../src/can/core/setting.c
HOST = ../src/can/host.cpp ../src/can/device/md.cpp ../src/can/device/controller.cpp \
       ../src/can/device/servo.cpp ../src/can/device/pin.cpp

test: test_core test_node test_host
	./test_core
	./test_node
	./test_host

test_core: test_core.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

test_node: test_node.c ../src/can/node.c $(CORE)
	$(CC) $(CFLAGS) -o $@ test_node.c ../src/can/node.c $(CORE)

test_host: test_host.cpp $(HOST) $(CORE)
	$(CC) $(CFLAGS) -c $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ test_host.cpp $(HOST) md.o setting.o

clean:
	rm -f test_core test_node test_host *.o

.PHONY: test clean
