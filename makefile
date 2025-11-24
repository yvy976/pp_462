CC = mpicc
CFLAGS = -fopenmp -Wall -O3 -lm

SRC_DIR = src
BIN_DIR = bin

# Find all .c files in src
SRC = $(shell find $(SRC_DIR) -type f -name '*.c')

# Strip src/ and .c to get executable names
EXE = $(patsubst $(SRC_DIR)/%.c,%,$(SRC))

all: $(EXE)

%: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $<

clean:
	rm -f $(BIN_DIR)/*

