PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/youtube-stream
DATADIR ?= $(PREFIX)/share/youtube-stream

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=

X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 xext 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 xext 2>/dev/null || printf '%s\n' '-lX11 -lXext')
JSON_CFLAGS := $(shell $(PKG_CONFIG) --cflags json-c 2>/dev/null)
JSON_LIBS := $(shell $(PKG_CONFIG) --libs json-c 2>/dev/null || printf '%s\n' '-ljson-c')
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s\n' '-lcurl')

BUILD_DIR := build
STREAM := $(BUILD_DIR)/youtube-stream
API := $(BUILD_DIR)/youtube-stream-api
OUTLINE := $(BUILD_DIR)/youtube-stream-outline

.PHONY: all check install uninstall clean

all: $(STREAM) $(API) $(OUTLINE)

$(BUILD_DIR):
	@mkdir -p $@

$(STREAM): src/youtube-stream.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(JSON_CFLAGS) -o $@ $< $(JSON_LIBS)

$(API): src/youtube-stream-api.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS) -o $@ $< $(JSON_LIBS) $(CURL_LIBS)

$(OUTLINE): src/youtube-stream-outline.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(X11_CFLAGS) -o $@ $< $(X11_LIBS)

check: all
	./$(OUTLINE) --help >/dev/null
	./$(STREAM) --help >/dev/null
	./$(API) --help >/dev/null

install: all
	install -Dm755 $(STREAM) "$(DESTDIR)$(BINDIR)/youtube-stream"
	install -Dm755 $(API) "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-api"
	install -Dm755 $(OUTLINE) "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-outline"
	install -Dm644 README.md "$(DESTDIR)$(DATADIR)/README.md"
	install -Dm644 config.example "$(DESTDIR)$(DATADIR)/config.example"
	@printf 'Installed %s\n' "$(DESTDIR)$(BINDIR)/youtube-stream"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/youtube-stream"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-api"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-outline"

clean:
	rm -rf $(BUILD_DIR)
