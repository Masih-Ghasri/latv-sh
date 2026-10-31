CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -O2
TARGET = latv-sh
SRC = latv_sh.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) test_output.tmp /tmp/latv-demo.txt /tmp/latv_test.txt /tmp/x

test: $(TARGET)
	python3 tests/test_runner.py

.PHONY: all clean test
