CC      = clang
TARGET  = claude-paint
SRCDIR  = src
SRCS    = $(wildcard $(SRCDIR)/*.c)
OBJS    = $(SRCS:.c=.o)

CFLAGS  = -std=c11 -Wall -Wextra -g \
          $(shell pkg-config --cflags raylib sqlite3)
LDFLAGS = $(shell pkg-config --libs raylib sqlite3) \
          -framework IOKit -framework Cocoa \
          -framework OpenGL -framework CoreVideo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run
