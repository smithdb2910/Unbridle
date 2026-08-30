CC = gcc
CFLAGS = -Wall -Wextra -fPIC -std=c11 -I./include
LDFLAGS = -ldl -lpthread

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)

COMMON_SRC = src/common/config.c src/common/utils.c src/common/socket_manager.c
HOOK_SRC = src/hook/hook.c src/hook/proxy.c src/hook/udp_manipulation.c
GUI_SRC = src/gui/main.c

BUILD_DIR = build
LIB_NAME = libdiscord-unbridle.so
GUI_NAME = unbridle

.PHONY: all clean install uninstall

all: $(BUILD_DIR)/$(LIB_NAME) $(BUILD_DIR)/$(GUI_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/$(LIB_NAME): $(COMMON_SRC) $(HOOK_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $(COMMON_SRC) $(HOOK_SRC) $(LDFLAGS)

$(BUILD_DIR)/$(GUI_NAME): $(COMMON_SRC) $(GUI_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ $(COMMON_SRC) $(GUI_SRC) $(GTK_LIBS) -lpthread

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -d $(DESTDIR)/usr/lib/unbridle
	install -m 755 $(BUILD_DIR)/$(LIB_NAME) $(DESTDIR)/usr/lib/unbridle/
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(BUILD_DIR)/$(GUI_NAME) $(DESTDIR)/usr/bin/

uninstall:
	rm -rf $(DESTDIR)/usr/lib/unbridle
	rm -f $(DESTDIR)/usr/bin/$(GUI_NAME)
