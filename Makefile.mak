#
# Makefile for compiling with Microsoft C/++ and nmake
# nmake /f Makefile.mak
#

socketpipe.exe: socketpipe-win.c
	cl -o socketpipe.exe -W3 -Ox socketpipe-win.c mswsock.lib Ws2_32.lib Kernel32.lib
