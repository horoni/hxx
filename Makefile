PHONY   := all run clean valgrind
PROJECT := hxx

SRC_DIR   := src
OBJ_DIR   := obj

SRCS := $(shell find $(SRC_DIR) -name '*.c' -or -name '*.s')
OBJS := $(SRCS:%=$(OBJ_DIR)/%.o)

CC       := gcc
AS       := as
WARNINGS := -Wall -Wextra -Wpedantic
CFLAGS   := $(WARNINGS) -std=gnu99 -Iinclude/ -O3
LDFLAGS  += -lcurses -flto

ifeq ($(MSAN),1)
$(info [SAN] MSan enabled)
CFLAGS  += -g -fPIE -fsanitize=memory -fno-omit-frame-pointer
LDFLAGS += -fPIE -pie -fsanitize=memory
else ifeq ($(ASAN),1)
$(info [SAN] ASan enabled)
CFLAGS  += -g -fsanitize=address,undefined,leak -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined,leak
endif

RM    := rm -f
RMF   := rm -rf
RMDIR := rmdir

all: run

run: $(PROJECT)
	@./$(PROJECT) $(UARG)

$(PROJECT): $(OBJS)
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.c.o: %.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo [CC] $@
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.s.o: %.s | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo [AS] $@
	@$(AS) -c $< -o $@

$(OBJ_DIR):
	@mkdir -p $@

clean:
	@$(RMF) $(OBJ_DIR) $(PROJECT)

valgrind: $(PROJECT)
	@valgrind ./$(PROJECT)


