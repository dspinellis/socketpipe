NAME=socketpipe
PREFIX?=/usr/local
INSTALL?=install

all: $(NAME)

$(NAME): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $?

install: $(NAME)
	$(INSTALL) -s $(NAME) $(PREFIX)/bin
	gzip -c $(NAME).1 >$(PREFIX)/man/man1/$(NAME).1.gz

socketpipe.exe: socketpipe-win.c
	cl -o socketpipe.exe -W3 -Ox socketpipe-win.c mswsock.lib Ws2_32.lib Kernel32.lib

clean:
	rm -f $(NAME) $(NAME).o
