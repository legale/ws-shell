# websocket server
BIN = ws
SRC = main.c

CC     = gcc
FLAGS += -pipe -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections -Wl,--gc-sections
DEFINE += -DLINUX
INCLUDE = -I /usr/src/linux-headers-$(shell uname -m) -I /usr/include/
OBJ     = $(SRC:.c=.o)
CFLAGS  += $(FLAGS) $(INCLUDE) $(DEFINE)
LDFLAGS += -L/usr/lib
LDLIBS = -lc -lwebsockets -lssl -lpthread -lcap -lcrypto

# Default build (without debug symbols)
all: $(BIN) static

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $(BIN)

# Static version target
static: CFLAGS += -static
static: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $(BIN)_static

# Debug version target
debug: CFLAGS += -ggdb3
debug: clean $(BIN)_debug

$(BIN)_debug: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $(BIN)_debug

clean:
	rm -rf $(OBJ) $(BIN) $(BIN)_static $(BIN)_debug *.o *.so core *.core *~
