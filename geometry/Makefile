CC ?= cc
WARNINGS = -Wall -Wextra -Wpedantic -Werror
CFLAGS ?= -std=c11 -O2 $(WARNINGS)

TEST_BINARY = geometry_test
SANITIZE_BINARY = geometry_test_sanitize

.PHONY: all test sanitize clean

all: $(TEST_BINARY)

$(TEST_BINARY): geometry.c geometry.h geometry_test.c
	$(CC) $(CFLAGS) geometry.c geometry_test.c -o $@

test: $(TEST_BINARY)
	./$(TEST_BINARY)

sanitize: geometry.c geometry.h geometry_test.c
	$(CC) -std=c11 -O1 -g $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer geometry.c geometry_test.c -o $(SANITIZE_BINARY)
	./$(SANITIZE_BINARY)

clean:
	rm -f $(TEST_BINARY) $(SANITIZE_BINARY)
