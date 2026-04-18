CC      = clang
TARGET  = claude-paint
SRCDIR  = src
SRCS    = $(wildcard $(SRCDIR)/*.c)
MSRCS   = $(wildcard $(SRCDIR)/*.m)
OBJS    = $(SRCS:.c=.o) $(MSRCS:.m=.o)

CFLAGS  = -std=c11 -Wall -Wextra -g \
          $(shell pkg-config --cflags raylib sqlite3)
LDFLAGS = $(shell pkg-config --libs raylib sqlite3) \
          -framework IOKit -framework Cocoa \
          -framework OpenGL -framework CoreVideo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

HDRS    = $(wildcard $(SRCDIR)/*.h)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(SRCDIR)/%.o: $(SRCDIR)/%.m $(HDRS)
	$(CC) -std=c11 -Wall -Wextra -g -x objective-c \
	      $(shell pkg-config --cflags raylib sqlite3) -c $< -o $@

clean:
	rm -f $(SRCDIR)/*.o $(TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run
