/* 
 * (C) Copyright 2003 Diomidis Spinellis
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
 * $Id: socketpipe.c,v 1.3 2003/08/27 14:10:33 dds Exp $
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static char *progname;

static void
usage(const char *msg)
{
	fprintf(stderr, "%s: %s\n", progname, msg);
	fprintf(stderr, "usage:\t%s [-i|o|r|l { command [args ...] }]\n", progname);
	fprintf(stderr, "\t(must specify a -l and a -r command and at least one of -i or -o)\n");
#ifdef DEBUG
	fprintf(stderr, "\t%s -s host port command [args ...]\n", progname);
	fprintf(stderr, "\t(typically automatically executed at the remote end)\n");
#endif
	exit(1);
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

static void *
xmalloc(size_t n)
{
	void *p = malloc(n);

	if (p)
		return p;
	else
		fatal("out of memory");
}

/* Program argument vectors */
static char **inputv, **outputv, **remotev, **loginv;

/*
 * Set the input, output, remote, and login vectors based on
 * the arguments supplied
 */
static void
parse_arguments(char *argv[])
{
	char **p, **start;
	char ***result;
	int nest;
	
	for (p = argv + 1; *p;) {
		/* Require a single character option */
		if (p[0][0] != '-' || !p[0][1] || p[0][2])
			usage("single character option expected");
		switch (p[0][1]) {
		case 'i': result = &inputv; break;
		case 'o': result = &outputv; break;
		case 'r': result = &remotev; break;
		case 'l': result = &loginv; break;
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
		*result = (char **)xmalloc(sizeof(char *) * (p - start + 1));
		memcpy(*result, start, sizeof(char *) * (p - start));
		(*result)[p - start] = NULL;
		p++;
	}
}

/*
 * Client invocation interface
 * Run the remote command on the remote machine connecting it to
 * the local input and/or output processes.
 */
static void
client(char *argv[])
{
	struct sockaddr_in loc_addr, rem_addr;
	int sockfd, newsockfd = -1;
	int addr_len;
	char hostname[MAXHOSTNAMELEN];
	char portname[20];
	char **rloginv, **p, **rp;
	int count;
	int lastpid;			/* Last child in the pipeline */
	int inpid, outpid;
	int nwait = 0;			/* Children to wait for */
	int exitstatus;

	parse_arguments(argv);
	if (!remotev)
		usage("must specify remote command");
	if (!loginv)
		usage("must specify remote login method");
	if (!inputv && !outputv)
		usage("must specify a local input or output process");

	/* Create socket to remote end */
	if (gethostname(hostname, sizeof(hostname)) < 0)
		fatal("gethostname failed: %s", strerror(errno));

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		fatal("socket allocation failed: %s", strerror(errno));

	bzero((char *)&loc_addr, sizeof(loc_addr));
	loc_addr.sin_family = AF_INET;
	loc_addr.sin_addr.s_addr = htonl(INADDR_ANY);	/* Us */
	loc_addr.sin_port = 0;				/* Kernel assigned port */

	if (bind(sockfd, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0)
		fatal("bind to local address failed: %s", strerror(errno));

	if (listen(sockfd, 1) < 0)
		fatal("listen failed: %s", strerror(errno));

	addr_len = sizeof(loc_addr);
	if (getsockname(sockfd, (struct sockaddr *)&loc_addr, &addr_len) < 0)
		fatal("getsockname failed: %s", strerror(errno));
	snprintf(portname, sizeof(portname), "%d", ntohs(loc_addr.sin_port));

	/* Merge loginv and remotev into rloginv */
	count = 4;		/* ... socketpipe -s host port ... */
	for (p = loginv; *p; p++)
		count++;
	for (p = remotev; *p; p++)
		count++;
	rp = rloginv = (char **)xmalloc(sizeof(char *) * (count + 1));
	for (p = loginv; *p; p++)
		*rp++ = *p;
	*rp++ = "socketpipe";
	*rp++ = "-s";
	*rp++ = hostname;
	*rp++ = portname;
	for (p = remotev; *p; p++)
		*rp++ = *p;
	*rp = NULL;


	/* Run the remote process to obtain the incoming socket fd */
	switch (lastpid = fork()) {
	case -1:
		/* Failure */
		fatal("fork failed: %s", strerror(errno));
	default:
		/* Parent; accept a connection */
		if ((newsockfd = accept(sockfd, (struct sockaddr *)&rem_addr, &addr_len)) < 0)
			fatal("accept failed: %s", strerror(errno));
		nwait++;
		break;
	case 0:
		/* Child; remotely execute the command specified */
		/* 
		 * ssh messes with stdout converting the parent end
		 * to non-blocking I/O.  We therefore close it here.
		 */
                close(STDOUT_FILENO);
		close(sockfd);
		if (execvp(rloginv[0], rloginv) < 0)
			fatal("execution of %s failed: %s", rloginv[0], strerror(errno));
		/* NOTREACHED */
	}

	/* Run the I/O commands redirected to newsockfd */
	inpid = outpid = -1;

	/* Run the input generation process */
	if (inputv)
		switch (inpid = fork()) {
		case -1:
			/* Failure */
			fatal("fork failed: %s", strerror(errno));
		default:
			/* Parent */
			nwait++;
			break;
		case 0:
			/* Child */
			if (dup2(newsockfd, STDOUT_FILENO) < 0)
				fatal("input process output redirection failed: %s", strerror(errno));
			if (close(newsockfd) < 0)
				fatal("socket close failed: %s", strerror(errno));
			if (execvp(inputv[0], inputv) < 0)
				fatal("execution of %s failed: %s", inputv[0], strerror(errno));
			/* NOTREACHED */
		}

	/* Run the output processing process */
	if (outputv)
		switch (outpid = lastpid = fork()) {
		case -1:
			/* Failure */
			fatal("fork failed: %s", strerror(errno));
		default:
			/* Parent */
			nwait++;
			break;
		case 0:
			/* Child */
			if (dup2(newsockfd, STDIN_FILENO) < 0)
				fatal("output process input redirection failed: %s", strerror(errno));
			if (close(newsockfd) < 0)
				fatal("socket close failed: %s", strerror(errno));
			if (execvp(outputv[0], outputv) < 0)
				fatal("execution of %s failed: %s", outputv[0], strerror(errno));
			/* NOTREACHED */
		}

	/* Wait for all our children to terminate */
	while (nwait) {
		int status;
		pid_t pid;

		if ((pid = wait(&status))  == -1)
			fatal("wait failed: %s", strerror(errno));
		nwait--;
		if (pid == lastpid) {
			if (WIFEXITED(status))
				exitstatus = WEXITSTATUS(status);
			if (WIFSIGNALED(status))
				exitstatus = WTERMSIG(status) + 128;
		}
		if (pid == inpid)
			if (shutdown(newsockfd, SHUT_WR) < 0)
				fatal("shutdown(SHUT_WR) failed: %s", strerror(errno));
		if (pid == outpid)
			if (shutdown(newsockfd, SHUT_RD) < 0)
				fatal("shutdown(SHUT_RD) failed: %s", strerror(errno));
	}
	exit(exitstatus);
}

/*
 * Server invocation interface.
 * Run as the remote server executing the specified command and
 * connecting back to the client.
 */
static void
server(char *argv[])
{
	int port;
	struct sockaddr_in rem_addr;
	int sock;
	char *endptr;
	struct hostent *h;
	in_addr_t inaddr;


	port = (int)strtol(argv[3], &endptr, 10);
	if (*argv[3] == 0 || *endptr != 0)
		fatal("bad port specification: %s", argv[3]);
		
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		fatal("socket allocation failed: ", strerror(errno));

	bzero((char *)&rem_addr, sizeof(rem_addr));
	rem_addr.sin_port = htons(port);

	if (inet_aton(argv[2], &rem_addr.sin_addr) <= 0) {
		if ((h = gethostbyname2(argv[2], AF_INET)) == NULL)
			fatal("gethostbyname(%s) failed: %s", argv[2], hstrerror(h_errno));
		memcpy(&rem_addr.sin_addr, h->h_addr_list[0], sizeof(rem_addr.sin_addr));
	}
	rem_addr.sin_family = AF_INET;

	if (connect(sock, (struct sockaddr *)&rem_addr, sizeof(rem_addr)) < 0)
		fatal("connect(%s) failed: %s", argv[2], strerror(errno));

	/* Redirect I/O to the socket */
	if (dup2(sock, STDIN_FILENO) < 0)
		fatal("input redirection failed: %s", strerror(errno));

	if (dup2(sock, STDOUT_FILENO) < 0)
		fatal("output redirection failed: %s", strerror(errno));

	if (execvp(argv[4], argv + 4) < 0)
		fatal("exec(%s) failed: %s", argv[4], strerror(errno));
	/* NOTREACHED */
}

int
main(int argc, char *argv[])
{
	progname = argv[0];
	if (!argv[1])
		usage("no arguments specified");
	if (strcmp(argv[1], "-s") == 0)
		server(argv);
	else
		client(argv);
}