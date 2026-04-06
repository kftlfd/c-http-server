SRC = src/main.c

TARGET = server

CFLAGS = -Wall -Wextra -std=c11

build:
	gcc $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm $(TARGET)
