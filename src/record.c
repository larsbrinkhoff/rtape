/*
 * This file implements the record stream on top of a stdio file.
 *
 * Read side functions are:
 *	recop(f)	return opcode of current record (EOF at end).
 *	reclength(f)	return data size in current record.
 *	recread(f, buf, n)	read from current record.
 *	recchar(f)	get next char (EOF on last).
 *
 * Write side functions are:
 *	recstart(f, op, len);
 *	recwrite(f, op, buf, len);
 *	recwchar(f, c);			Write a char in the record.
 *
 * No protocol identifier is received or sent.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <chaos.h>

#include "record.h"

#define salloc(s) ((struct s *)malloc(sizeof(struct s)))
#define sfree(s) free((char *)s)

static int
recerr(char *s)
{
	fprintf(stderr, "Record stream error: %s\n", s);
	abort();
	exit(1);
}

struct record_stream *
recopen(int f, int mode)
{
	struct record_stream *rp;

	if ((rp = salloc(record_stream)) != NULL) {
		rp->r_rfp = rp->r_wfp = NULL;
		if (mode == 0 || mode == 2)
			if ((rp->r_rfp = fdopen(f, "r")) == NULL) {
				sfree(rp);
				return NULL;
			}
		if (mode == 1 || mode == 2)
			if ((rp->r_wfp = fdopen(f, "w")) == NULL) {
				if (rp->r_rfp) {
					int rfd = dup(f);

					fclose(rp->r_rfp);
					dup2(rfd, f);
					close (rfd);
				}
				sfree(rp);
				return NULL;
			}
		rp->r_rlen = rp->r_wlen = 0;
		if (rp->r_wfp) {
			fwrite(RECMAGIC, strlen(RECMAGIC), 1, rp->r_wfp);
			recforce(rp);
		}
		{
			char magic[sizeof(RECMAGIC)];
			fread(magic, strlen(RECMAGIC), 1, rp->r_rfp);
			magic[strlen(RECMAGIC)] = '\0';
			if (strcmp(RECMAGIC, magic))
				recerr("Bad version of record protocol");
		}
	}
	return rp;
}

int
recforce(struct record_stream *rp)
{
	fflush(rp->r_wfp);
	ioctl(fileno(rp->r_wfp), CHIOCFLUSH, 0);
}

int
recclose(struct record_stream *rp)
{
	int myfd;
	int dfd;

	if (rp->r_rfp) {
		myfd = fileno(rp->r_rfp);
		dfd = dup(myfd);
		fclose(rp->r_rfp);
		dup2(dfd, myfd);
		close(dfd);
	}
	if (rp->r_wfp) {
		myfd = fileno(rp->r_wfp);
		dfd = dup(myfd);
		fclose(rp->r_wfp);
		dup2(dfd, myfd);
		close(dfd);
	}
}

int
recop(struct record_stream *rp)
{
	int c, c1;
	char op;

	if (rp->r_rfp == NULL)
		recerr("Not open for reading");
	if (rp->r_rlen != 0)
		while (rp->r_rlen--)
			if (getc(rp->r_rfp) == EOF)
				return EOF;
	if ((c = getc(rp->r_rfp)) == EOF)
		return EOF;
	else {
		op = c & 0377;;
		if ((c = getc(rp->r_rfp)) == EOF ||
		    (c1 = getc(rp->r_rfp)) == EOF)
			recerr("EOF after opcode");
		rp->r_rlen = (c1 & 0377) | ((c & 0377) << 8);
	}
	return op;
}

int
reclength(struct record_stream *rp)
{
	return rp->r_rlen;
}

int
recread(struct record_stream *rp, char *buf, int len)
{
	int n;

	n = rp->r_rlen > len ? len : rp->r_rlen;
	if (n == 0)
		return EOF;
	if (fread(buf, n, 1, rp->r_rfp) != 1)
		recerr("Record too short");
	rp->r_rlen -= n;
	return n;
}

int
recchar(struct record_stream *rp)
{
	int c;

	if (rp->r_rlen)
		if ((c = getc(rp->r_rfp)) == EOF)
			recerr("Record too short");
		else
			rp->r_rlen--;
	else
		c = EOF;
	return c;
}

int
recstart(struct record_stream *rp, int op, int len)
{
	if (rp->r_wfp == NULL)
		recerr("Not open for writing");
	if (rp->r_wlen != 0)
		recerr("Unfinished output record");
	if (putc(op, rp->r_wfp) == EOF ||
	    putc((len >> 8) & 0377, rp->r_wfp) == EOF ||
	    putc(len & 0377, rp->r_wfp) == EOF)
		return EOF;
	rp->r_wlen = len;
	return 0;
}

int
recwchar(struct record_stream *rp, int c)
{
	if (rp->r_wlen == 0)
		recerr("Uninitialized output record");
	if (putc(c, rp->r_wfp) == EOF)
		return EOF;
	rp->r_wlen--;
	return 0;
}

int
recwrite(struct record_stream *rp, int op, char *buf, int len)
{
	if (recstart(rp, op, len) == EOF)
		return EOF;
	if (fwrite(buf, len, 1, rp->r_wfp) != 1)
		return EOF;
	rp->r_wlen = 0;
	return 0;
}

int
recrfileno(struct record_stream *rs)
{
	return fileno(rs->r_rfp);
}

int
recwfileno(struct record_stream *rs)
{
	return fileno(rs->r_wfp);
}

int
recflush(struct record_stream *rs)
{
	fflush(rs->r_wfp);
}
