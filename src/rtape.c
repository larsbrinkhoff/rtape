#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mtio.h>
#include <sys/ioctl.h>
#include <chaos.h>
#ifdef vax
#ifdef BSD42
#include <vaxuba/tmreg.h>
#include <vaxuba/tsreg.h>
#include <vaxuba/utreg.h>
#include <vaxmba/htreg.h>
#include <vaxmba/mtreg.h>
#else
#include <sys/tmreg.h>
#include <sys/htreg.h>
#include <sys/tsreg.h>
#endif /* BSD42 */
#endif /* vax */
#include "record.h"
/*
 * UNIX RTAPE server.
 */
#define LOGIN		1
#define MOUNT		2
#define PROBE		3
#define READ		4
#define WRITE		5
#define	REWIND		6
#define REWIND_SYNC	7
#define OFFLINE		8
#define FILEPOS		9
#define BLOCKPOS	10
#define WRITE_EOF	12
#define CLOSE		13
#define LOGIN_RESPONSE	33
#define DATA		34
#define EOFREAD		35
#define STATUS		36

#define DLEN		16
#define MAXSTRING	100
struct tape_status	{
	char	t_version;	/* protocol version */
	char	t_probeid[2];	/* Id in corresponding PROBE */
	char	t_read[3];	/* Number of blocks read */
	char	t_skipped[3];	/* Number of blocks skipped */
	char	t_discarded[3];	/* Numbert of writes discarded */
	char	t_lastop;	/* last opcode received */
	char	t_density[2];	/* Density in BPI */
	char	t_retries[2];	/* number of retries in last op. */
	char	t_namelength;	/* length of next string */
	char	t_drive[DLEN];	/* Drive name in use. */
	char	t_solicited:1;	/* This status was asked for */
	char	t_bot:1;	/* At BOT */
	char	t_pasteot:1;	/* Past EOT */
	char	t_eof:1;	/* Last op reached EOF */
	char	t_nli:1;	/* Not logged in */
	char	t_mounted:1;	/* Tape is mounted */
	char	t_message:1;	/* Error message follows */
	char	t_harderr:1;	/* Hard error encountered */
	char	t_softerr:1;	/* Soft errors encountered */
	char	t_offline:1;	/* Drive is offline */
	char	t_string[MAXSTRING];	/* Error message */
} ts;

#define MAXMOUNT	CHMAXDATA	/* Maximum size of mount command */
#define MAXBLOCK	(30*1024)	/* Maximum tape buffer size */
#define MINBLOCK	10		/* ????? */
#define DEFBLOCK	4096		/* Default tape buffer size */
#define DEFDENS		1600		/* Default tape density */
#define DEFDRIVE	0		/* Default tape drive number */

int options;	/* Mount options bit mask */
#define ONOREWIND	1
#define OOFFLINE	2
struct option {
	char *o_name;
	int	o_bit;
} optab[] = {
	{"NOREWIND",	ONOREWIND},
	{"OFFLINE",	OOFFLINE},
	{NULL, 	0},
};

void tmount();
int tclose(), tprobe(), tread(), twrite(), trewind(),
    trewsync(), toffline(), tfile(), tblock(), tweof();
struct command {
	int c_num;
	int (*c_func)();
} commands[] = {
/*	c_num		c_func */
	{CLOSE,		tclose},
	{MOUNT,		tmount},
	{PROBE,		tprobe},
	{READ,		tread},
	{WRITE,		twrite},
	{REWIND,	trewind},
	{REWIND_SYNC,	trewsync},
	{OFFLINE,	toffline},
	{FILEPOS,	tfile},
	{BLOCKPOS,	tblock},
	{WRITE_EOF,	tweof},
	{0,		0},
};
	
/* Physical tape operations */
#define TMOUNT		0
#define TWRITE		1
#define TREAD		2
#define TREWIND		3
#define TOFFLINE	4
#define TWEOF		5
#define TFILE		6
#define TBLOCK		7

int errno;
char lastop;			/* last opcode recieved */
char lastop2;			/* last op before lastop */
int tapefd = -1;		/* File descriptor for tape drive */
int nread = -1;			/* Length of last record read */
char tapepath[256];		/* Pathname of tape drive */
int tapemode;			/* Open mode for tape */
int tdensity;			/* Density of tape */
int maxblock;			/* Size of tapebuf */
char *tapebuf;			/* Tape buffer */
int nwritten;			/* Number of records written */
int loggedin;
int aborting;			/* A fatal error has occurred */
int faking_tape;		/* faking a tape drive with disk */
struct record_stream *rs;	/* The controlling record stream from the user */
char *recbad = "Record stream protocol error";
struct mtget mst;
struct mtop mtop;
#define newop(op)	lastop2 = lastop;lastop = op

void getstat(void);
    
int main(argc, argv)
int argc;
char **argv;
{
	/* RFC already accepted the connection, so its open */

	register struct command *cp;
	register int op;

	faking_tape = 1;

	if (argc == 2 && strcmp(argv[1], "listen") == 0) {
		close(0);
		close(1);
		chlisten("RTAPE", 2, 0, 0);
		dup(0);
		chwaitfornotstate(0, CSLISTEN);
		ioctl(0, CHIOCACCEPT, 0);
	}
	close(2);
	creat("/tmp/RTAPE.log", 0666);

	if ((rs = recopen(0, 2)) == NULL) {
		chreject(0, "Can't open record stream");
		exit(1);
	}
	tclose();
	while ((op = recop(rs)) != EOF) {
		for (cp = commands; cp->c_num; cp++)
			if (op == cp->c_num)
				break;
		if (cp->c_num == 0) {
			chreject(0, "Bad tape protocol opcode");
			exit(1);
		}
		(*cp->c_func)();
	}
	tclose();
	exit(0);
}

char *
skip(cp)
register char *cp;
{
	while (*cp && *cp != ' ')
		cp++;
	if (*cp)
		*cp++ = '\0';
	return cp;
}
char *
skipnl(cp)
register char *cp;
{
	while (*cp && *cp != CHNL)
		cp++;
	if (*cp)
		*cp++ = '\0';
	return cp;
}

/*
 * Mount request - open a tape drive - close previous one if it was open.
 */
void tmount()
{
	register char *cp;
	char *type, *reel, *drive, *blocksize, *optstring, *message, *density;
	char mountcom[MAXMOUNT];
	int len, drivenum, densinc;

	tclose();
	newop(TMOUNT);
	if ((len = reclength(rs)) >= MAXMOUNT) {
		tstatus(0, 0, "Mount record too long");
		return;
	}
	if (recread(rs, mountcom, len) != len)
		fatal(recbad);
	mountcom[len] = '\0';
	cp = mountcom;
	type = cp;
	cp = skip(cp);
	reel = cp;
	cp = skip(cp);
	drive = cp;
	cp = skip(cp);
	blocksize = cp;
	cp = skip(cp);
	density = cp;
	cp = skip(cp);
	optstring = cp;
	cp = skipnl(cp);
	if (parseoptions(optstring, &options)) {
		return;
	}
	message = cp;
	/*
	 * The write mode is set to 2 even if only open for writing since the
	 * device drivers for tapes ALWAYS write EOF's on a tape at close time
	 * even if the last operation was not a write - this loses big
	 * if we write a tape, rewind ourselves and close, which is a
	 * perfectly legal operation under this protocol.
	 */
	tapemode = cp == NULL ? -1 :
		   !strcmp(type, "READ") ? 0 :
		   !strcmp(type, "WRITE") ? 2 :
		   !strcmp(type, "BOTH") ? 2 : -1;
	if (tapemode == -1) {
		tstatus(0, 0, "Bad mount type: %s", tapemode);
		return;
	}
	if (drive[0] == '\0')
		drivenum = 0;
	else if (drive[1] || !isdigit(drive[0]) || drive[0] > '3') {
		tstatus(0, 0, "Drive can only be single digits 0-3: %s",
			tapemode);
		return;
	} else
		drivenum = drive[0] - '0';
	if (blocksize[0] == '\0')
		maxblock = DEFBLOCK;
	else if (blocksize[0] == '0' && blocksize[1] == '\0')
		maxblock = MAXBLOCK;
	else
		maxblock = atoi(blocksize);
	if (maxblock < MINBLOCK || maxblock > MAXBLOCK) {
		tstatus(0, 0, "Unacceptable maximum block size: %s", blocksize);
		return;
	}
	if (density[0] == '\0')
		tdensity = DEFDENS;
	else
		tdensity = atoi(density);
	if (tdensity == 1600)
		densinc = 8;
	else if (tdensity == 800)
		densinc = 0;
	else {
		tstatus(0, 0, "Unacceptable density: %s", density);
		return;
	}
	if (options & ONOREWIND)
		densinc += 4;
/*
	if (message[0])
		syslog(LOG_INFO, "Remote tape request: %s", message);
*/
	sprintf(tapepath, "/dev/rmt%d", drivenum + densinc);
	if (faking_tape)
	    sprintf(tapepath, "/tmp/tape.rmt%d", drivenum + densinc);
	if (tapebuf)
		free(tapebuf);
	if ((tapebuf = malloc(maxblock)) == NULL) {
		tstatus(0, 0, "Can't allocate buffer of %d bytes", maxblock);
		return;
	}
	if ((tapefd = open(tapepath, tapemode)) < 0) {
		switch (errno) {
		case ENOENT:
			tstatus(0, 0, "Nonexistent drive: %0", drivenum);
			break;
		case ENXIO:
			tstatus(0, 0, "Drive %d unavailable", drivenum);
			break;
		case EIO:
			tstatus(0, 0, "Drive open error (Offline, No Write ring etc.) on drive: %d",
				drivenum);
			break;
		case EPERM:
			tstatus(0, 0, "No permission to access drive: %s",
				drive);
			break;			
		default:
			tstatus(0, 0, "System error %d while opening drive: %d",
				errno, drivenum);
		}
		return;
	}
	{
		int pathlen = strlen(tapepath);
		ts.t_namelength = DLEN < pathlen ? DLEN : pathlen;
		strncpy(ts.t_drive, tapepath, DLEN);
	}
	ts.t_mounted = 1;
	itoc2(tdensity, ts.t_density);
}

int parseoptions(string, bits)
char *string;
int *bits;
{
	register char *cp, *op;
	register struct option *o;

	for (*bits = 0, op = string; *op; op = cp) {
		cp = skip(op);
		for (o = optab; o->o_name; o++)
			if (strcmp(o->o_name, op) == 0) {
				*bits |= o->o_bit;
				break;
			}
		if (o->o_name == NULL) {
			tstatus(0, 0, "Bad option: %s", op);
			return 1;
		}
	}
	return 0;
}


/*
 * Return a status packet.
 */
int tprobe()
{
	register int id;

	if (reclength(rs) != 2)
		fatal("Probe record had invalid length");
	id = recchar(rs);
	id |= recchar(rs) << 8;
	tstatus(id, NULL);
}

/*
 * Send a status back.
 */
/* VARARGS 2 */
int tstatus(id, hard, string, a1, a2)
int id;
int hard;
char *string;
int a1;
int a2;
{
	itoc2(id, ts.t_probeid);
	ts.t_lastop = lastop;
	itoc2(0, ts.t_retries);
	ts.t_solicited = id != 0;
	if (ts.t_mounted) {
		if (hard)
			ts.t_harderr = 1;
		getstat();
	}
	if (string) {
		ts.t_message = 1;
		sprintf(ts.t_string, string, a1, a2);
	}
	if (!ts.t_message)
		ts.t_string[0] = '\0';
	if (recwrite(rs, STATUS, (char *)&ts,
		     (int)((struct tape_status *)0)->t_string +
			strlen(ts.t_string)) == EOF)
		fatal(recbad);
	recforce(rs);
}
int clearstatus()
{
	ts.t_message = ts.t_harderr = 0;
}
int terror()
{
	tstatus(0, 1, errno == EIO ? NULL : "System error: %d", errno);
}

int itoc2(i, cp)
register int i;
register char *cp;
{
	*cp++ = i;
	i >>= 8;
	*cp++ = i;
}
int itoc3(l, cp)
register long l;
register char *cp;
{

	*cp++ = l;
	l >>= 8;
	*cp++ = l;
	l >>= 8;
	*cp++ = l;
}

/*
 * Read some blocks.
 */
int tread()
{
	register int nrecs;

	checkmount();
	if (tapemode == 1)
		fatal("READ command when not mounted for reading");
	checkeof(0);
	if (reclength(rs) == 0)
		nrecs = 0;
	else
		if ((nrecs = numrec()) <= 0)
			fatal("Bad number of blocks in READ command");
	newop(TREAD);
	do {
		off_t navail;
		if (ioctl(recrfileno(rs), FIONREAD, &navail) < 0)
			fatal("Bad ioctl");
		if (navail)
			break;
		clearstatus();
		if ((nread = read(tapefd, tapebuf, maxblock)) <= 0) {
			if (nread == 0)
				recwrite(rs, EOFREAD, "", 0);
			else
				terror();
			break;
		}
		if (recwrite(rs, DATA, tapebuf, nread) == EOF)
			fatal(recbad);
	} while (--nrecs);
	recforce(rs);
}
int numrec()
{
	register int n = 0;
	register int c;
	register int sign = 0;

	while ((c = recchar(rs)) != EOF)
		if (!isdigit(c))
			if (c == '-')
				sign = 1;
			else
				fatal("Non digit in numeric record");
		else {
			n *= 10;
			n += c - '0';
		}
	if (sign)
		n = - n;
	return n;
}
int checkmount()
{
	if (tapefd < 0)
		fatal("Illegal operation when not mounted");
}
/*
 * Write a record.
 */
int twrite()
{
	register int length;
	register int n;

	checkmount();
	if (tapemode < 1)
		fatal("WRITE command when not mounted for writing");
	if ((length = reclength(rs)) <= 0 || length > maxblock)
		fatal("WRITE record length unacceptable");
	if (recread(rs, tapebuf, length) != length)
		fatal(recbad);
	newop(TWRITE);
	clearstatus();
	if ((n = write(tapefd, tapebuf, length)) != length)
		terror();
	else
		nwritten++;
}
/*
 * Check whether we should write tape marks.
 * This should be called whenever we are about to violate the
 * integrity of the tape.
 * The willrewind flag indicates that we are about to rewind the
 * tape anyway, so extra positioning after writing tape marks is
 * unnecessary.
 */
int checkeof(willrewind)
int willrewind;
{
	if (lastop == TWRITE) {
		tweof();
		/* What to do with errors here? */
		tweof();
		/* What to do with errors here? */
		filespace(-2);
	} else if (lastop == TWEOF && lastop2 != TWEOF) {
		tweof();
		/* What to do with errors here? */
		filespace(-1);
	}
	lastop = 0;
}		

void doioctl(struct mtop *pmtop)
{
	if (faking_tape) {
		switch (pmtop->mt_op) {
		case MTREW:
		    lseek(tapefd, (off_t)0, SEEK_SET);
		    break;
		case MTWEOF:
		{
		    off_t size = lseek(tapefd, (off_t)0, SEEK_CUR);
		    ftruncate(tapefd, (size_t)size);
		}
		    break;
		case MTBSR:
		    lseek(tapefd,(off_t)(-pmtop->mt_count*maxblock), SEEK_CUR);
		    break;
		case MTFSR:
		    lseek(tapefd,(off_t)(pmtop->mt_count*maxblock), SEEK_CUR);
		    break;
		}
		return;
	}

	if (ioctl(tapefd, MTIOCTOP, pmtop) < 0)
		terror();
}

/*
 * trewind - rewind the tape. If last tape op was a write, write eof's first.
 */
int trewind()
{
	checkmount();
	checkeof(1);
	newop(TREWIND);
	mtop.mt_op = MTREW;
	mtop.mt_count = 1;
	clearstatus();
	doioctl(&mtop);
}
/*
 * tweof - write an EOF mark.
 */
int tweof()
{
	checkmount();
	if (tapemode < 1)
		fatal("EOF attempted when not open for writing");
	mtop.mt_op = MTWEOF;
	mtop.mt_count = 1;
	newop(TWEOF);
	clearstatus();
	doioctl(&mtop);
}
/*
 * toffline
 */
int toffline()
{
	checkmount();
	trewind();
	mtop.mt_op = MTOFFL;
	mtop.mt_count = 1;
	newop(TOFFLINE);
	clearstatus();
	doioctl(&mtop);
}
int trewsync()
{
	trewind();
}
int tfile()
{
	register int nfiles;

	checkmount();
	if ((nfiles = numrec(rs)) == 0)
		fatal("Bad number of files to space over");
	checkeof(0);
	clearstatus();
	newop(TFILE);
	filespace(nfiles);
}
int filespace(nfiles)
int nfiles;
{
	if (nfiles < 0) {
		mtop.mt_op = MTBSF;
		mtop.mt_count = - nfiles;
	} else {
		mtop.mt_op = MTFSF;
		mtop.mt_count = nfiles;
	}
	doioctl(&mtop);
}
int tblock()
{
	register int nblocks;

	checkmount();
	if ((nblocks = numrec(rs)) == 0)
		fatal("Bad number of blocks to space over");
	checkeof(0);
	if (nblocks < 0) {
		sleep(10);	/* This is a crock because of a bug in the driver */
		mtop.mt_op = MTBSR;
		mtop.mt_count = - nblocks;
	} else {
		sleep(10);
		mtop.mt_op = MTFSR;
		mtop.mt_count = nblocks;
	}
	newop(TBLOCK);
	clearstatus();
	doioctl(&mtop);
}
int fatal(s, a)
char *s;
int a;
{
	char err[100];

	if (!aborting) {
		aborting++;
		sprintf(err, s, a);
		tstatus(0, "Fatal error in tape server: %s", err);
		tclose();
	}
	abort();
	exit(1);
}
/*
 * Close the tape gracefully if possible.
 */
int tclose()
{
	if (tapefd >= 0) {
		if (options & OOFFLINE)
			toffline();
		else if (options & ONOREWIND)
			checkeof(0);
		else
			trewind();
		close(tapefd);
		tapefd = -1;
	}
	ts.t_version = 1;
	ts.t_namelength = 0;
	ts.t_mounted = 0;
	ts.t_string[0] = '\0';
	lastop = 0;
	itoc2(0, ts.t_density);
}
/*
 * Get status from crufty device dependent bits.
 */
void getstat()
{
	register struct tape_status *t = &ts;
	register struct mtget *m = &mst;

	if (faking_tape) {
		t->t_softerr = 0;
		t->t_bot = 0;
		t->t_pasteot = 0;
		t->t_offline = 0;
		t->t_eof = 0;
		return;
	}

	if (ioctl(tapefd, MTIOCGET, &mst) < 0)
		fatal("Ioctl error");
	t->t_softerr = 0;
	switch (m->mt_type) {
#ifdef vax
	case MT_ISTM:
		t->t_bot = (m->mt_erreg & TMER_BOT) != 0;
		t->t_pasteot = (m->mt_erreg & TMER_EOT) != 0;
		t->t_offline = (m->mt_erreg & TMER_SELR) == 0;
		t->t_eof = (m->mt_erreg & TMER_EOF) != 0;
		break;
	case MT_ISHT:
		t->t_bot = (m->mt_dsreg & HTDS_BOT) != 0;
		t->t_pasteot = (m->mt_dsreg & HTDS_EOT) != 0;
		t->t_offline = (m->mt_dsreg & HTDS_MOL) == 0;
		t->t_eof = (m->mt_dsreg & HTDS_TM) != 0;
		break;
	case MT_ISTS:
		t->t_bot = (m->mt_erreg & TS_BOT) != 0;
		t->t_pasteot = (m->mt_erreg & TS_EOT) != 0;
		t->t_offline = (m->mt_erreg & TS_ONL) == 0;
		t->t_eof = (m->mt_erreg & TS_TMK) != 0;
		break;
#ifdef 41ABSD
	case MT_ISMT:
		t->t_bot = (m->mt_dsreg & MTDS_BOT) != 0;
		t->t_pasteot = (m->mt_dsreg & MTDS_EOT) != 0;
		t->t_offline = (m->mt_dsreg & MTDS_ONL) == 0;
		t->t_eof = (m->mt_dsreg & MTER_TM) != 0;
		break;
	case MT_ISUT:
		t->t_bot = (m->mt_dsreg & UTDS_BOT) != 0;
		t->t_pasteot = (m->mt_dsreg & UTDS_EOT) != 0;
		t->t_offline = (m->mt_dsreg & UTDS_MOL) == 0;
		t->t_eof = (m->mt_dsreg & UTDS_TM) != 0;
		break;
#endif 41ABSD
	default:
		fatal("Unknown tape drive type: %d", m->mt_type);
#endif /* vax */
#ifdef linux
	default:
		t->t_bot = GMT_BOT(m->mt_gstat) != 0;
		t->t_pasteot = GMT_EOT(m->mt_gstat) != 0;
		t->t_offline = GMT_ONLINE(m->mt_gstat) == 0;
		t->t_eof = GMT_EOF(m->mt_gstat) != 0;
		break;
#endif

		/* NOTREACHED */
	}
}
