NAME=socketpipe
UXHOST=spiti
SSH=plink
VERSION=1.9
DIR=$(NAME)-$(VERSION)
SRC_BALL=$(DIR).tar.gz
DOC=$(NAME).txt $(NAME).pdf $(NAME).html
WEBDIR=/cygdrive/c/dds/pubs/web/home/sw/unix/$(NAME)

$(NAME): $(NAME).c
	$(CC) $(CFLAGS) -o $@ $?

$(NAME).ps: $(NAME).1
	$(SSH) $(UXHOST) groff -man -Tps <$? > $@

$(NAME).txt: $(NAME).1
	$(SSH) $(UXHOST) groff -man -Tascii <$? | $(SSH) $(UXHOST) col -b > $@

$(NAME).pdf: $(NAME).ps
	ps2pdf $? $@

$(NAME).html: $(NAME).1
	$(SSH) $(UXHOST) groff -mhtml -Thtml -man <$? | sed -e 's/&minus;/-/g;s/&bull;/\&#8226;/g' >$@

src-tarball: $(SRC_BALL)

$(SRC_BALL): $(DOC) $(NAME).1 $(NAME).c Makefile.dist
	-cmd /c rd /q/s "$(DIR)"
	mkdir $(DIR)
	cp $(NAME).1 $(NAME).c $(NAME).pdf $(NAME).html $(NAME).txt ChangeLog.txt socketpipe-win.c $(DIR)
	cp Makefile.dist $(DIR)/Makefile
	for i in $(NAME).1 $(NAME).c Makefile ; do perl /usr/local/bin/lf.bat $(DIR)/$$i ; done
	tar czvf $(SRC_BALL) $(DIR)
	cmd /c rd /q/s "$(DIR)"

socketpipe.exe: socketpipe-win.c
	cl -o socketpipe.exe -W3 -Ox socketpipe-win.c mswsock.lib Ws2_32.lib Kernel32.lib

web: $(DOC) $(SRC_BALL) socketpipe.exe
	-chmod 666 $(WEBDIR)/$(NAME).c $(WEBDIR)/$(NAME).1 $(WEBDIR)/socketpipe-win.c
	cp -f $(SRC_BALL) $(NAME).c $(NAME).1 $(NAME).pdf $(NAME).html $(NAME).jpg ChangeLog.txt socketpipe-win.c socketpipe.exe $(WEBDIR)
	sed -e "s/VERSION/${VERSION}/" index.html >${WEBDIR}/index.html
