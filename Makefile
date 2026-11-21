# Builds and tests the C core on Linux/macOS (the Swift package is built by SwiftPM/Xcode).
CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
INC      = -ISources/CFlashIO/include
SRC      = Sources/CFlashIO/platform.c Sources/CFlashIO/store.c
BUILD    = .build-c

.PHONY: test bench clean
test: $(BUILD)/test_flashio
	./$(BUILD)/test_flashio

$(BUILD)/test_flashio: $(SRC) tests/c/test_flashio.c Sources/CFlashIO/include/flashio.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(INC) $(SRC) tests/c/test_flashio.c -o $@ -lpthread

bench: $(BUILD)/bench
	./$(BUILD)/bench

$(BUILD)/bench: $(SRC) tests/c/bench.c Sources/CFlashIO/include/flashio.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 $(INC) $(SRC) tests/c/bench.c -o $@ -lpthread

clean:
	rm -r -f .build-c
