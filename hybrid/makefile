CC = mpicc
CFLAGS = -fopenmp -Wall -Wextra -Iinclude -O3 -ggdb
SRCS := $(wildcard src/*.c)
TARGET = bin/wc

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

.PHONY: test
test: $(TARGET) ../omp/text_inputs/random_3200000.txt
	$^

.PHONY: clean
clean:
	rm -f $(TARGET)

