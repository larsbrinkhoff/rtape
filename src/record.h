#define RECMAGIC "RECORD STREAM VERSION 1\215"

struct	record_stream {
	FILE	*r_rfp;
	FILE	*r_wfp;
	int	r_rlen;
	int	r_wlen;
};

extern struct record_stream *recopen(int f, int mode);
extern int recforce(struct record_stream *rp);
extern int recclose(struct record_stream *rp);
extern int recop(struct record_stream *rp);
extern int reclength(struct record_stream *rp);
extern int recread(struct record_stream *rp, char *buf, int len);
extern int recchar(struct record_stream *rp);
extern int recstart(struct record_stream *rp, int op, int len);
extern int recwchar(struct record_stream *rp, int c);
extern int recwrite(struct record_stream *rp, int op, char *buf, int len);
extern int recrfileno(struct record_stream *rs);
extern int recwfileno(struct record_stream *rs);
extern int recflush(struct record_stream *rs);
