PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/youtube-stream
DATADIR ?= $(PREFIX)/share/youtube-stream

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra
X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 xext 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 xext 2>/dev/null || printf '%s\n' '-lX11 -lXext')

OUTLINE := build/youtube-stream-outline

.PHONY: all check install uninstall clean

all: $(OUTLINE)

$(OUTLINE): youtube-stream-outline.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(X11_CFLAGS) -o $@ $< $(X11_LIBS)

check: all
	bash -n youtube-stream
	python3 -m py_compile youtube-stream-api
	./$(OUTLINE) --help >/dev/null

install: all
	install -Dm755 youtube-stream "$(DESTDIR)$(BINDIR)/youtube-stream"
	install -Dm755 youtube-stream-api "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-api"
	install -Dm755 $(OUTLINE) "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-outline"
	install -Dm644 README.md "$(DESTDIR)$(DATADIR)/README.md"
	install -Dm644 config.example "$(DESTDIR)$(DATADIR)/config.example"
	@printf 'Installed %s\n' "$(DESTDIR)$(BINDIR)/youtube-stream"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/youtube-stream"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-api"
	rm -f "$(DESTDIR)$(LIBEXECDIR)/youtube-stream-outline"

clean:
	rm -rf build
