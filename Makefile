TARGET = server
BUILD_DIR = build

SRC = \
	src/main.c \
	src/server.c \
	src/http.c \
	src/fs.c \
	src/buffer.c \
	src/log.c

# map src/foo.c -> build/foo.o
OBJ = $(SRC:src/%.c=$(BUILD_DIR)/%.o)

CC = gcc

F_POSIX		= -D_POSIX_C_SOURCE=200809L	# for portability
F_WARNINGS	= -Wall -Wextra
F_C_STD		= -std=c11
F_DEPS		= -MMD -MP # auto-generate .d files

CFLAGS = $(F_C_STD) $(F_POSIX) $(F_WARNINGS) $(F_DEPS)

# default target
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET)

# compile step
# `@` = hide command output
# `$<` = first dependency (e.g. src/main.c)
# `$@` = target (e.g. src/main.o)
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -r $(BUILD_DIR) $(TARGET)

# include .d files if exist (and recompile if .h change)
# `-include` = don't error if missing
-include $(OBJ:.o=.d)
