CC = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2
LDFLAGS = -lnvidia-ml -lfuse3

SRC = main.c
BIN = nvfs
SERVICE = nvidia-fuse.service

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMDDIR = /etc/systemd/system

.PHONY: all install uninstall clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: all
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -Dm644 $(SERVICE) $(SYSTEMDDIR)/$(SERVICE)
	systemctl daemon-reload
	systemctl enable --now $(SERVICE)

uninstall:
	systemctl stop $(SERVICE) || true
	systemctl disable $(SERVICE) || true
	rm -f $(SYSTEMDDIR)/$(SERVICE)
	systemctl daemon-reload
	rm -f $(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)
