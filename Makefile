NAME=socketpipe
PREFIX?=/usr/local
INSTALL?=install

all: $(NAME)

$(NAME): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $?

install: $(NAME)
	$(INSTALL) -s $(NAME) $(PREFIX)/bin
	gzip -c $(NAME).1 >$(PREFIX)/man/man1/$(NAME).1.gz

clean:
	rm -f $(NAME) $(NAME).o
