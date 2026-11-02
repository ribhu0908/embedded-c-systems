CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
INC     := -Iinclude
SRC     := src/dbg_heap.c src/ringbuf.c src/sw_timer.c
TEST    := tests/test_main.c
BUILD   := build

.PHONY: all test sanitize demo arm clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: $(SRC) $(TEST) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $^ -o $@

test: $(BUILD)/tests
	./$(BUILD)/tests

demo: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) src/dbg_heap.c examples/demo.c -o $(BUILD)/demo
	./$(BUILD)/demo

# AddressSanitizer + UndefinedBehaviorSanitizer on the host
sanitize: | $(BUILD)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -fno-sanitize-recover=all $(INC) $(SRC) $(TEST) -o $(BUILD)/tests_san
	./$(BUILD)/tests_san

# Proves the library builds for a real Cortex-M4 target (needs arm-none-eabi-gcc)
arm: | $(BUILD)
	arm-none-eabi-gcc -std=c11 -Os -Wall -Wextra -Werror -mcpu=cortex-m4 -mthumb \
	      $(INC) -c src/dbg_heap.c -o $(BUILD)/dbg_heap.o
	arm-none-eabi-gcc -std=c11 -Os -Wall -Wextra -Werror -mcpu=cortex-m4 -mthumb \
	      $(INC) -c src/ringbuf.c -o $(BUILD)/ringbuf.o
	arm-none-eabi-gcc -std=c11 -Os -Wall -Wextra -Werror -mcpu=cortex-m4 -mthumb \
	      $(INC) -c src/sw_timer.c -o $(BUILD)/sw_timer.o
	arm-none-eabi-size $(BUILD)/*.o

clean:
	rm -rf $(BUILD)
