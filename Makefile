CC       ?= cc
PREFIX   ?= /usr/local
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS  ?=

# Try to detect ncurses via pkg-config (wide-char build preferred).
NCURSES_PKG := $(shell pkg-config --exists ncursesw 2>/dev/null && echo ncursesw || \
                       (pkg-config --exists ncurses 2>/dev/null && echo ncurses))

ifneq ($(NCURSES_PKG),)
    CFLAGS  += -DUSE_NCURSES $(shell pkg-config --cflags $(NCURSES_PKG))
    LDFLAGS += $(shell pkg-config --libs $(NCURSES_PKG))
else
    # Fall back to plain -lncursesw / -lncurses if pkg-config metadata is missing.
    CFLAGS  += -DUSE_NCURSES
    LDFLAGS += -lncursesw
endif

all: memmon

memmon: memmon.c
	$(CC) $(CFLAGS) -o $@ memmon.c $(LDFLAGS)

install: memmon
	install -Dm755 memmon $(DESTDIR)$(PREFIX)/bin/memmon
	install -Dm644 memmon.service $(DESTDIR)/etc/systemd/system/memmon.service
	@echo ""
	@echo "Installed. Next steps:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now memmon.service"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/memmon
	rm -f $(DESTDIR)/etc/systemd/system/memmon.service

clean:
	rm -f memmon

.PHONY: all install uninstall clean
