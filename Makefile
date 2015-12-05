NAME=socketpipe
PREFIX?=/usr/local
INSTALL?=install
TEST_HOST?=spiti
TEST_FILE=/usr/share/dict/words

all: $(NAME)

$(NAME): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $?

install: $(NAME)
	$(INSTALL) -s $(NAME) $(PREFIX)/bin
	gzip -c $(NAME).1 >$(PREFIX)/man/man1/$(NAME).1.gz

clean:
	rm -f $(NAME) $(NAME).o

test:
	./$(NAME) -i \{ dd if=$(TEST_FILE) \} \
		-l \{ ssh $(TEST_HOST) \} -r \{ dd \} \
		-o \{ dd of=test-words \}
	diff -q $(TEST_FILE) test-words
	./$(NAME) -i \{ dd \} \
		-l \{ ssh $(TEST_HOST) \} -r \{ dd \} \
		-o \{ dd of=test-words \} <$(TEST_FILE)
	diff -q $(TEST_FILE) test-words
	./$(NAME) -i \{ dd if=$(TEST_FILE) \} \
		-l \{ ssh $(TEST_HOST) \} -r \{ dd \} \
		-o \{ dd \} >test-words
	diff -q $(TEST_FILE) test-words
	./$(NAME) -i \{ dd \} \
		-l \{ ssh $(TEST_HOST) \} -r \{ dd \} \
		-o \{ dd \} <$(TEST_FILE) >test-words
	diff -q $(TEST_FILE) test-words
	rm test-words
