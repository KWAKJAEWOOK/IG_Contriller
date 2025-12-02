# ==== ubuntu Makefile ====
CC     := gcc
INC    := -I. -I./global
CFLAGS := -O2 -Wall -Wextra -std=gnu11 $(INC)
LDFLAGS := -pthread
LIBS    := -lrt -lm

SRC_COMMON := global/shm_func.c \
              global/msg_func.c \
              global/CommData.c \
              global/cJSON.c \
              global/logger.c
OBJ_COMMON := $(SRC_COMMON:.c=.o)

SRC_START      := start.c
SRC_IG         := IG_Server_Manager.c
SRC_LED        := LED_Manager.c
SRC_M30        := M30_Manager.c
SRC_CONN       := Connection_Manager.c
SRC_BH1750	   := BH1750_Manager.c
SRC_END        := end.c

OBJ_START      := $(SRC_START:.c=.o)
OBJ_IG         := $(SRC_IG:.c=.o)
OBJ_LED        := $(SRC_LED:.c=.o)
OBJ_M30        := $(SRC_M30:.c=.o)
OBJ_CONN       := $(SRC_CONN:.c=.o)
OBJ_BH1750	   := $(SRC_BH1750:.c=.o)
OBJ_END        := $(SRC_END:.c=.o)

TARGETS := start.out \
           IG_Server_Manager.out \
           LED_Manager.out \
           M30_Manager.out \
           Connection_Manager.out \
		   BH1750_Manager.out \
           end.out

.PHONY: all postcopy clean

all: $(TARGETS) postcopy

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

start.out: $(OBJ_START) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

IG_Server_Manager.out: $(OBJ_IG) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

LED_Manager.out: $(OBJ_LED) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

M30_Manager.out: $(OBJ_M30) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

Connection_Manager.out: $(OBJ_CONN) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

BH1750_Manager.out: $(OBJ_BH1750) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

end.out: $(OBJ_END) $(OBJ_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

postcopy: $(TARGETS)
	@mkdir -p exe
	@cp -f *.out exe/

clean:
	rm -f *.o global/*.o *.out exe/*.out