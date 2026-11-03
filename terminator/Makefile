CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE
LDFLAGS = -lX11 -lpthread -lrt

SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = terminator

.PHONY: all clean run run-process test

all: $(TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET)

run-process: $(TARGET)
	./$(TARGET) useProcess=True

clean:
	rm -rf $(OBJ_DIR) $(TARGET) test_runner

TEST_OBJS = obj/parser.o obj/history.o obj/completion.o obj/script.o obj/config.o obj/jobcontrol.o obj/buffer.o obj/shell.o obj/builtins.o obj/multiwatch.o

test_runner: tests/test_unit.c $(TEST_OBJS)
	$(CC) $(CFLAGS) tests/test_unit.c $(TEST_OBJS) $(LDFLAGS) -o $@

test: $(TARGET) test_runner
	@chmod +x tests/*.sh 2>/dev/null || true
	./test_runner
	./tests/test_all.sh
