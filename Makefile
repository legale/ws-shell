# websocket server
BIN = ws
SRC = main.c

CC     = gcc
FLAGS  += -pipe -Wall -Wextra -Wno-unused-parameter -ggdb3
DEFINE += -DLINUX
INCLUDE = -I $(MUSL)/usr/include/
OBJ     = $(SRC:.c=.o)
CFLAGS  += $(FLAGS) $(INCLUDE) $(DEFINE)
LDFLAGS += -L/usr/local/lib
LDLIBS = -lc -lwebsockets -lssl -lpthread -lcap -lcrypto

# Add flags for static build
STATIC_LDFLAGS = -static

all: $(BIN) static

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $(BIN)

# Static version target
static: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) $(OBJ) $(LDLIBS) -o $(BIN)_static

clean:
	rm -rf $(OBJ) $(BIN) $(BIN)_static *.o *.so core *.core *~
