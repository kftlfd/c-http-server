SRC = src/main.c

TARGET = server

CFLAGS = -D_POSIX_C_SOURCE=200809L -Wall -Wextra -std=c11

build:
	gcc $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm $(TARGET)
