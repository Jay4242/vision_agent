# Compiler and flags
CC = gcc
CFLAGS = -Wall -g # -Wall for all warnings, -g for debugging info
LDFLAGS = -lX11 -lcurl -lcjson -lssl -lcrypto
TARGET = vision_agent
SRCS = main.c

# Default target
all: $(TARGET)

# Link the program
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	@echo "Compilation successful! Run with ./%s" "$(TARGET)"

# Clean up build artifacts
clean:
	rm -f $(TARGET)

# Phony targets
.PHONY: all clean
