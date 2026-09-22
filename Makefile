CC       ?= cc
PREFIX   ?= /usr/local
SYSCONFDIR ?= /etc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS  ?=

# `make WERROR=1` turns warnings into errors (used by CI).
ifeq ($(WERROR),1)
    CFLAGS += -Werror
endif

# VERSION is the single place to bump the app version (see scripts/bump-version.sh
# to also update debian/changelog and the man page in one step).
VERSION := $(shell cat VERSION)
BASE_CFLAGS := $(CFLAGS) -DVERSION=\"$(VERSION)\" -DSYSCONFDIR=\"$(SYSCONFDIR)\"

# zlib (log compression, reading .gz logs in --report)
ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS   := $(shell pkg-config --libs zlib 2>/dev/null || echo -lz)

# Try to detect ncurses via pkg-config (wide-char build preferred).
# `make NO_NCURSES=1` builds without it (plain/once/daemon modes only).
ifeq ($(NO_NCURSES),1)
    NCURSES_CFLAGS :=
    NCURSES_LIBS   :=
else
    NCURSES_PKG := $(shell pkg-config --exists ncursesw 2>/dev/null && echo ncursesw || \
                           (pkg-config --exists ncurses 2>/dev/null && echo ncurses))
    ifneq ($(NCURSES_PKG),)
        NCURSES_CFLAGS := -DUSE_NCURSES $(shell pkg-config --cflags $(NCURSES_PKG))
        NCURSES_LIBS   := $(shell pkg-config --libs $(NCURSES_PKG))
    else
        # Fall back to plain -lncursesw if pkg-config metadata is missing.
        NCURSES_CFLAGS := -DUSE_NCURSES
        NCURSES_LIBS   := -lncursesw
    endif
endif

all: memmon

memmon: src/memmon.c VERSION
	$(CC) $(BASE_CFLAGS) $(ZLIB_CFLAGS) $(NCURSES_CFLAGS) -o $@ src/memmon.c $(LDFLAGS) $(ZLIB_LIBS) $(NCURSES_LIBS)

# Unit tests compile the source directly (without main() and ncurses) so
# static functions can be tested; the CLI tests drive the real binary
# against fixture /proc and /sys trees.
tests/test_memmon: tests/test_memmon.c src/memmon.c VERSION
	$(CC) $(BASE_CFLAGS) -Wno-unused-function $(ZLIB_CFLAGS) -o $@ tests/test_memmon.c $(LDFLAGS) $(ZLIB_LIBS)

check: memmon tests/test_memmon
	./tests/test_memmon
	./tests/cli-tests.sh ./memmon

test: check

install: memmon
	install -Dm755 memmon $(DESTDIR)$(PREFIX)/bin/memmon
	install -Dm644 man/memmon.1 $(DESTDIR)$(PREFIX)/share/man/man1/memmon.1
	install -d $(DESTDIR)/etc/systemd/system
	sed 's|/usr/bin/memmon|$(PREFIX)/bin/memmon|g' systemd/memmon.service \
	    > $(DESTDIR)/etc/systemd/system/memmon.service
	chmod 644 $(DESTDIR)/etc/systemd/system/memmon.service
	install -Dm644 systemd/memmon.sysusers $(DESTDIR)/etc/sysusers.d/memmon.conf
	@if [ -e $(DESTDIR)$(SYSCONFDIR)/memmon/memmon.conf ]; then \
	    echo "Keeping existing $(DESTDIR)$(SYSCONFDIR)/memmon/memmon.conf"; \
	else \
	    install -Dm644 config/memmon.conf $(DESTDIR)$(SYSCONFDIR)/memmon/memmon.conf; \
	fi
	install -Dm644 completions/memmon.bash $(DESTDIR)$(PREFIX)/share/bash-completion/completions/memmon
	install -Dm644 completions/_memmon $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_memmon
	@if [ -z "$(DESTDIR)" ] && command -v systemd-sysusers >/dev/null 2>&1; then \
	    systemd-sysusers /etc/sysusers.d/memmon.conf; \
	fi
	@echo ""
	@echo "Installed. Next steps:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now memmon.service"

uninstall:
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
	    systemctl disable --now memmon.service 2>/dev/null || true; \
	fi
	rm -f $(DESTDIR)$(PREFIX)/bin/memmon
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/memmon.1
	rm -f $(DESTDIR)/etc/systemd/system/memmon.service
	rm -f $(DESTDIR)/etc/sysusers.d/memmon.conf
	rm -f $(DESTDIR)$(SYSCONFDIR)/memmon/memmon.conf
	-rmdir $(DESTDIR)$(SYSCONFDIR)/memmon 2>/dev/null
	rm -f $(DESTDIR)$(PREFIX)/share/bash-completion/completions/memmon
	rm -f $(DESTDIR)$(PREFIX)/share/zsh/site-functions/_memmon
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
	    systemctl daemon-reload 2>/dev/null || true; \
	fi
	@echo "Uninstalled. Log files in /var/log/memmon and the 'memmon' system user were left in place."

clean:
	rm -f memmon tests/test_memmon
	rm -rf tests/tmp

.PHONY: all check test install uninstall clean
