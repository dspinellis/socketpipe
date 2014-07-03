/*
 * socketpipe - zero overhead remote process plumbing
 * Microsoft Windows version
 *
 * (C) Copyright 2003-2005 Diomidis Spinellis
 *
 * Permission to use, copy, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appear in all copies and that
 * both that copyright notice and this permission notice appear in
 * supporting documentation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <io.h>
#include <process.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <ws2spi.h>
#include <sys/types.h>

#define MAXHOSTNAMELEN 1024

#define STDIN_FILENO 0
#define STDOUT_FILENO 1

static char *progname;

static void
usage(const char *msg)
{
	fprintf(stderr, "%s: %s\n", progname, msg);
	fprintf(stderr, "usage:\t%s [-b] [-i|o|r|l { command [args ...] }]\n", progname);
	fprintf(stderr, "\t(must specify a -l and a -r command and at least one of -i or -o)\n");
#ifdef DEBUG
	fprintf(stderr, "\t%s -s host port command [args ...]\n", progname);
	fprintf(stderr, "\t(typically automatically executed at the remote end)\n");
#endif
	exit(1);
}


/* Return as a string the error description for err */
static char *
wstrerror(LONG err)
{
	static LPVOID lpMsgBuf;

	if (lpMsgBuf)
		LocalFree(lpMsgBuf);
	FormatMessage(
	    FORMAT_MESSAGE_ALLOCATE_BUFFER |
	    FORMAT_MESSAGE_FROM_SYSTEM |
	    FORMAT_MESSAGE_IGNORE_INSERTS,
	    NULL,
	    err,		// GetLastError() does not seem to work reliably here
	    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
	    (LPTSTR) &lpMsgBuf,
	    0,
	    NULL
	);
	return lpMsgBuf;
}

/*
 * Report failure due to a fatal error as directed
 * by fmt and its arguments and exit the program
 */
static void
fatal(const char *fmt, ...)
{
	va_list marker;

	fprintf(stderr, "%s: ", progname);
	va_start(marker, fmt);
	vfprintf(stderr, fmt, marker);
	va_end(marker);
	fputc('\n', stderr);
	exit(2);
}

/* Checked malloc */
static void *
xmalloc(size_t n)
{
	void *p = malloc(n);

	if (p)
		return p;
	else
		fatal("out of memory");
}

/* Program argument strings */
static char *inputv, *outputv, *remotev, *loginv;
static int batch = 0;

/*
 * Set the input, output, remote, and login strings based on
 * the arguments supplied
 */
static void
parse_arguments(char *argv[])
{
	char **p, **start, **s;
	char **result;
	int nest, len;

	for (p = argv + 1; *p; p++) {
		/* Require a single character option */
		if (p[0][0] != '-' || !p[0][1] || p[0][2])
			usage("single character option expected");
		switch (p[0][1]) {
		case 'i': result = &inputv; break;
		case 'o': result = &outputv; break;
		case 'r': result = &remotev; break;
		case 'l': result = &loginv; break;
		case 'b': batch = 1; continue;
		default: usage("invalid option");
		}
		if (!*++p || strcmp(*p, "{"))
			usage("opening block expected");
		start = ++p;
		nest = 1;
		for (;;) {
			if (!*p)
				usage("unterminated block");
			if (strcmp(*p, "{") == 0)
				nest++;
			if (strcmp(*p, "}") == 0)
				nest--;
			if (nest == 0)
				break;
			p++;
		}
		if (p - start == 0)
			usage("command can not be empty");
		len = 0;
		for (s = start; s != p; s++)
			len += strlen(*s) + 1;
		*result = (char *)xmalloc(len);
		**result = 0;
		for (s = start; s != p; s++) {
			strcat(*result, *s);
			if (s + 1 != p)
				strcat(*result, " ");
		}
	}
}

/*
 * Return an IFS socket. This can be used for ReadFile/WriteFile
 */
SOCKET
ifs_socket(int af, int type, int proto)
{
	unsigned long pblen = 0;
	SOCKET ret;
	WSAPROTOCOL_INFOW *pbuff;
	WSAPROTOCOL_INFOA pinfo;
	int nprotos, i, err;

	if (WSCEnumProtocols(NULL, NULL, &pblen, &err) != SOCKET_ERROR)
		fatal("No socket protocols available");
	if (err != WSAENOBUFS)
		fatal("WSCEnumProtocols failed: %s", wstrerror(err));
	pbuff = (WSAPROTOCOL_INFOW *)xmalloc(pblen);
	if ((nprotos = WSCEnumProtocols(NULL, pbuff, &pblen, &err)) == SOCKET_ERROR)
		fatal("WSCEnumProtocols failed: %s", wstrerror(err));
	for (i = 0; i < nprotos; i++) {
		if ((af != AF_UNSPEC && af != pbuff[i].iAddressFamily)
		    || (type != pbuff[i].iSocketType)
		    || (proto != 0 && pbuff[i].iProtocol != 0 &&
		    proto != pbuff[i].iProtocol))
			continue;
		if (!(pbuff[i].dwServiceFlags1 & XP1_IFS_HANDLES))
			continue;

		memcpy(&pinfo, pbuff + i, sizeof(pinfo));
		wcstombs(pinfo.szProtocol, pbuff[i].szProtocol, sizeof(pinfo.szProtocol));
		free(pbuff);
		if ((ret = WSASocket(af, type, proto, &pinfo, 0, 0)) == INVALID_SOCKET)
			fatal("WSASocket failed: %s", wstrerror(WSAGetLastError()));
		return ret;
	}
	fatal("No IFS socket provider found");
	/* NOTREACHED */
}

/*
 * Client invocation interface
 * Run the remote command on the remote machine connecting it to
 * the local input and/or output processes.
 */
static int
client(char *argv[])
{
	struct sockaddr_in loc_addr;
	SOCKET sockfd, newsockfd;
	int addr_len;
	char hostname[MAXHOSTNAMELEN];
	char portname[20];
	char *rloginv;
	PROCESS_INFORMATION rempi;			/* Last child in the pipeline */
	PROCESS_INFORMATION inpi, outpi;
	HANDLE err_proc;		/* Process from whick to retrieve the error code */
	DWORD nwait = 0;			/* Children to wait for */
	DWORD exitstatus, bytes, waitret;
	STARTUPINFO rstart;
	WSAOVERLAPPED overlap;
	char lpOutputBuf[1024];
	LINGER linger;
	struct hostent *h;
	int one = 1;

	parse_arguments(argv);
	if (!remotev)
		usage("must specify remote command");
	if (!loginv)
		usage("must specify remote login method");
	if (!inputv && !outputv)
		usage("must specify a local input or output process");

	/* Create socket to remote end */
	if (gethostname(hostname, sizeof(hostname)) < 0)
		fatal("gethostname failed: %s", wstrerror(WSAGetLastError()));
	if ((h = gethostbyname(hostname)) == NULL)
		fatal("gethostbyname(%s) failed: %s", hostname, wstrerror(WSAGetLastError()));
	strcpy(hostname, inet_ntoa((*(struct in_addr *)(h->h_addr_list[0]))));
	printf("Host %s is IP %s\n", h->h_name, hostname);

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		fatal("socket allocation failed: %s", wstrerror(WSAGetLastError()));

	memset((char *)&loc_addr, 0, sizeof(loc_addr));
	loc_addr.sin_family = AF_INET;
	loc_addr.sin_addr.s_addr = htonl(INADDR_ANY);	/* Us */
	loc_addr.sin_port = 0;				/* Kernel assigned port */

	if (bind(sockfd, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0)
		fatal("bind to local address failed: %s", wstrerror(WSAGetLastError()));

	if (listen(sockfd, 1) < 0)
		fatal("listen failed: %s", wstrerror(WSAGetLastError()));

	addr_len = sizeof(loc_addr);
	if (getsockname(sockfd, (struct sockaddr *)&loc_addr, &addr_len) < 0)
		fatal("getsockname failed: %s", wstrerror(WSAGetLastError()));
	sprintf(portname, "%d", ntohs(loc_addr.sin_port));

	/* Merge loginv and remotev into rloginv */
	rloginv = (char *)xmalloc(
		strlen(loginv) + 1 +
		strlen("socketpipe -s") + 1 +
		strlen(hostname) + 1 +
		strlen(portname) + 1 +
		strlen(remotev));
	strcpy(rloginv, loginv);
	strcat(rloginv, " socketpipe -s ");
	strcat(rloginv, hostname); strcat(rloginv, " ");
	strcat(rloginv, portname); strcat(rloginv, " ");
	strcat(rloginv, remotev);

	/* Remotely execute the command specified */
	/* XXX handle batch flag? */
	memset(&rstart, 0, sizeof(rstart));
	rstart.cb = sizeof(rstart);
	rstart.wShowWindow = SW_HIDE;

	if (!CreateProcess(NULL, rloginv, NULL, NULL, 0, NORMAL_PRIORITY_CLASS, NULL, NULL, &rstart, &rempi))
		fatal("execution of { %s } failed: %s", rloginv, wstrerror(GetLastError()));
	err_proc = rempi.hProcess;
	/* Accept a connection */
	/*
	 * It seems we have to create newsockfd through WSASocket/AcceptEx and
	 * and IFS socket for ReadFile/WriteFile redirection through it to work.
	 * Socket descriptors returned from socket/accept did not work.
	 */
	newsockfd = ifs_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	linger.l_onoff = 1;
	linger.l_linger = 60;
	if (setsockopt(newsockfd, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof(linger)) != 0)
		fatal("setsockopt(SO_LINGER) failed: %s", wstrerror(WSAGetLastError()));
	if (setsockopt(newsockfd, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(int)) < 0)
		fatal("setsockopt(SO_KEEPALIVE) failed: %s", wstrerror(WSAGetLastError()));
	memset(&overlap, 0, sizeof(overlap));
	if ((overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) == NULL)
		fatal("CreateEvent failed: %s", wstrerror(GetLastError()));
	if (!AcceptEx(sockfd, newsockfd, lpOutputBuf, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &bytes, &overlap) &&
	    WSAGetLastError() != ERROR_IO_PENDING)
		fatal("AcceptEx failed: %s", wstrerror(WSAGetLastError()));
	if ((waitret = WaitForMultipleObjects(1, &(overlap.hEvent), FALSE, INFINITE)) == WAIT_FAILED)
		fatal("WaitForMultipleObjects on AcceptEx failed: %s", wstrerror(GetLastError()));
	assert(waitret == WAIT_OBJECT_0);
	nwait++;
	/* Run the I/O commands redirected to newsockfd */
	inpi.hProcess = outpi.hProcess = INVALID_HANDLE_VALUE;

	/* Allow the socket to be inherited */
	if (!SetHandleInformation((HANDLE)newsockfd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		fatal("SetHandleInformation failed: %s", wstrerror(GetLastError()));
	/* Run the input generation process */
	if (inputv) {
		STARTUPINFO istart;

		nwait++;
		memset(&istart, 0, sizeof(istart));
		istart.cb = sizeof(istart);
		istart.wShowWindow = SW_HIDE;
		istart.dwFlags = STARTF_USESTDHANDLES;
		istart.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		istart.hStdOutput = (HANDLE)newsockfd;
		istart.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		if (!CreateProcess(NULL, inputv, NULL, NULL, 1, NORMAL_PRIORITY_CLASS, NULL, NULL, &istart, &inpi))
			fatal("execution of { %s } failed: %s", inputv, wstrerror(GetLastError()));
	}

	/* Run the output processing process */
	if (outputv) {
		STARTUPINFO istart;

		nwait++;
		memset(&istart, 0, sizeof(istart));
		istart.cb = sizeof(istart);
		istart.wShowWindow = SW_HIDE;
		istart.dwFlags = STARTF_USESTDHANDLES;
		istart.hStdInput = (HANDLE)newsockfd;
		istart.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		istart.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		if (!CreateProcess(NULL, outputv, NULL, NULL, 1, NORMAL_PRIORITY_CLASS, NULL, NULL, &istart, &outpi))
			fatal("execution of { %s } failed: %s", outputv, wstrerror(GetLastError()));
		err_proc = outpi.hProcess;
	}

	if (closesocket(newsockfd) != 0)
		fatal("closesocket failed: %s", wstrerror(GetLastError()));
	/* Wait for all our children to terminate */
	while (nwait) {
		HANDLE handles[3];
		DWORD nCount;

		/* Create array of objects we are waiting on */
		nCount = 0;
		if (rempi.hProcess != INVALID_HANDLE_VALUE) handles[nCount++] = rempi.hProcess;
		if (inpi.hProcess != INVALID_HANDLE_VALUE) handles[nCount++] = inpi.hProcess;
		if (outpi.hProcess != INVALID_HANDLE_VALUE) handles[nCount++] = outpi.hProcess;
		assert(nCount == nwait);
		if ((waitret = WaitForMultipleObjects(nCount, handles, FALSE, INFINITE)) == WAIT_FAILED)
			fatal("WaitForMultipleObjects failed: %s", wstrerror(GetLastError()));
		nwait--;

		if (handles[waitret] == err_proc) {
			if (!GetExitCodeProcess(err_proc, &exitstatus))
				fatal("GetExitCodeProcess failed: %s", wstrerror(GetLastError()));
		}

		if (handles[waitret] == rempi.hProcess)
			rempi.hProcess = INVALID_HANDLE_VALUE;
		else if (handles[waitret] == inpi.hProcess)
			inpi.hProcess = INVALID_HANDLE_VALUE;
		else if (handles[waitret] == outpi.hProcess)
			outpi.hProcess = INVALID_HANDLE_VALUE;
	}
	return (exitstatus);
}

/*
 * Server invocation interface.
 * Run as the remote server executing the specified command and
 * connecting back to the client.
 */
static int
server(char *argv[])
{
	short port;
	struct sockaddr_in rem_addr;
	int sock;
	char *endptr;
	struct hostent *h;
	LINGER linger;
	STARTUPINFO istart;
	PROCESS_INFORMATION proc;
	int i, len;
	char *cmdline;
	DWORD exitstatus, waitret;
	int one = 1;

	port = (short)strtol(argv[3], &endptr, 10);
	if (*argv[3] == 0 || *endptr != 0)
		fatal("bad port specification: %s", argv[3]);

	sock = ifs_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	linger.l_onoff = 1;
	linger.l_linger = 60;
	if (setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof(linger)) != 0)
		fatal("setsockopt(SO_LINGER) failed: %s", wstrerror(WSAGetLastError()));
	/* Allow the socket to be inherited */
	if (!SetHandleInformation((HANDLE)sock, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		fatal("SetHandleInformation failed: %s", wstrerror(GetLastError()));

	memset((char *)&rem_addr, 0, sizeof(rem_addr));
	rem_addr.sin_port = htons(port);

	if (isalpha(*argv[2])) {		/* host address is a name */
		if ((h = gethostbyname(argv[2])) == NULL)
			fatal("gethostbyname(%s) failed: %s", argv[2], wstrerror(WSAGetLastError()));
	} else  {
		unsigned int addr = inet_addr(argv[2]);
		h = gethostbyaddr((char *)&addr, 4, AF_INET);
	}

	memcpy(&rem_addr.sin_addr, h->h_addr_list[0], sizeof(rem_addr.sin_addr));
	rem_addr.sin_family = AF_INET;

	if (connect(sock, (struct sockaddr *)&rem_addr, sizeof(rem_addr)) < 0)
		fatal("connect(%s) failed: %s", argv[2], wstrerror(WSAGetLastError()));
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(int)) < 0)
		fatal("setsockopt(SO_KEEPALIVE) failed: %s", wstrerror(WSAGetLastError()));

	/* Redirect I/O to the socket */
	memset(&istart, 0, sizeof(istart));
	istart.cb = sizeof(istart);
	istart.wShowWindow = SW_HIDE;
	istart.dwFlags = STARTF_USESTDHANDLES;
	istart.hStdInput = (HANDLE)sock;
	istart.hStdOutput = (HANDLE)sock;
	istart.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	/* Create command line */
	len = 0;
	for (i = 4; argv[i]; i++)
		len += strlen(argv[i]) + 1;
	cmdline = (char *)xmalloc(len);
	*cmdline = 0;
	for (i = 4; argv[i]; i++) {
		strcat(cmdline, argv[i]);
		if (argv[i] + 1)
			strcat(cmdline, " ");
	}
	if (!CreateProcess(NULL, cmdline, NULL, NULL, 1, NORMAL_PRIORITY_CLASS, NULL, NULL, &istart, &proc))
		fatal("execution of { %s } failed: %s", inputv, wstrerror(GetLastError()));
	if ((waitret = WaitForMultipleObjects(1, &(proc.hProcess), FALSE, INFINITE)) == WAIT_FAILED)
		fatal("WaitForMultipleObjects failed: %s", wstrerror(GetLastError()));
	if (!GetExitCodeProcess(proc.hProcess, &exitstatus))
		fatal("GetExitCodeProcess failed: %s", wstrerror(GetLastError()));
	if (closesocket(sock) != 0)
		fatal("closesocket failed: %s", wstrerror(GetLastError()));
	return (exitstatus);
}

int
main(int argc, char *argv[])
{
	WSADATA wsaData;
	WORD v;

	progname = argv[0];
	v = MAKEWORD(2, 2);
	if (WSAStartup(v, &wsaData) != 0)
		fatal("WSAStartup failed: %s", wstrerror(GetLastError()));
	if (!argv[1])
		usage("no arguments specified");
	if (strcmp(argv[1], "-s") == 0)
		return server(argv);
	else
		return client(argv);
}
