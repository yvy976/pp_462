CC = mpicc
CFLAGS = -fopenmp -Wall -Wextra -Iinclude -O3
SRCS := $(wildcard src/*.c)
TARGET = bin/wc

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

.PHONY: clean
clean:
	rm -f $(TARGET)

