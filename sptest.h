/* sptest.h - PsiBoost's link test: the parts that are only arithmetic, so
 * they can be tested on a PC (test/sptest_host.c) against server/stdsvc.py.
 * Plain C.
 *
 * One test runs at 115200 and then at 230400, the same route and server,
 * two legs each: a download (chargen from your own server, RFC 864, checked
 * character by character; or a file of known size and checksum from a
 * public web server over plain HTTP), then echo (RFC 862 on your own server,
 * or a public TCP echo), both ways at once, checked byte by byte. Any
 * modem reaches them with a plain ATDT host:port, and Psion Internet with
 * an ordinary socket.
 */
#ifndef SPTEST_H
#define SPTEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- chargen: lines of 72 of the 95 printable characters, each line one
   further on, then CR LF ---- */
typedef struct {
	int line;               /* the line now, modulo 95 */
	int col;                /* 0..73 in it (72 and 73 are CR and LF) */
	int synced;             /* 0 until a whole line has been seen right */
	unsigned long bad;      /* bytes that were not what chargen sends */
	unsigned long good;
} sp_chargen;

void sp_chargen_init(sp_chargen* c);
/* checks n bytes; returns how many were wrong (it finds its place again
   at the next line) */
unsigned long sp_chargen_check(sp_chargen* c, const unsigned char* p, int n);

/* ---- echo: a pseudo-random stream of every byte value, sent and checked
   as it comes back; or, for a public echo that only passes text (tcpbin
   drops CR and some other bytes), lines of 64 of a 64-character set, each
   ended by LF ---- */
typedef struct {
	unsigned long tx;       /* the sender's generator */
	unsigned long rx;       /* the checker's */
	unsigned long sent, back, bad;
	int text;               /* 1: text lines */
	unsigned long txpos, rxpos;
} sp_echo;

void sp_echo_init(sp_echo* e, unsigned long seed);
void sp_echo_init_text(sp_echo* e, unsigned long seed);
void sp_echo_fill(sp_echo* e, unsigned char* p, int n);
unsigned long sp_echo_check(sp_echo* e, const unsigned char* p, int n);

/* ---- MD5 (RFC 1321), for a public file's published checksum ---- */
typedef struct {
	unsigned long a, b, c, d;
	unsigned long lo, hi;   /* bytes so far */
	unsigned char buf[64];
	int n;                  /* in buf */
} sp_md5;

void sp_md5_init(sp_md5* m);
void sp_md5_update(sp_md5* m, const unsigned char* p, int n);
void sp_md5_final(sp_md5* m, unsigned char out[16]);
/* 1 if out is the 32-hex-digit aHex (either case) */
int sp_md5_is(const unsigned char out[16], const char* aHex);

/* ---- a plain HTTP response: the header skipped, the body counted ---- */
typedef struct {
	int status;             /* 0 until the status line is in */
	long length;            /* Content-Length (or the Content-Range's whole), -1 not said */
	int header_done;
	unsigned long body;     /* body bytes so far */
	unsigned long bad;      /* body bytes past the length said */
	char head[600];         /* the header as it comes (the first 599 bytes) */
	int head_n;
	sp_md5 md5;
} sp_http;

void sp_http_init(sp_http* h);
/* feeds n bytes; returns how many of them were body (counted and hashed) */
int sp_http_feed(sp_http* h, const unsigned char* p, int n);
/* after the end: 1 if the body was whole (its length as said) */
int sp_http_whole(const sp_http* h);

/* ---- the command-mode check: a long line any Hayes modem takes and
   echoes, with only harmless settings in it ---- */
#define SP_LONG_LINE "ATE1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0"
/* 1 if aReply (all that came back) holds aCommand's echo and OK, whole */
int sp_reply_ok(const char* aCommand, const char* aReply, int aLen);

/* ---- results and the verdict ---- */
typedef struct {
	long baud;              /* 115200 or 230400 */
	int route;              /* 0 the modem route, 1 Psion Internet */
	int ran;                /* this half ran (else it was never reached) */
	int divider_ok;         /* the UART stayed at the speed asked for */
	int cmds, cmds_bad;     /* command-mode lines, and those not echoed whole */
	unsigned long down_bytes, down_ms, down_bad;   /* the download */
	unsigned long echo_bytes, echo_ms, echo_bad;   /* echo, bytes back */
	unsigned long rtt_ms;   /* the first echo byte's round trip (0: none) */
	int stalls;             /* 3 s with nothing coming in */
	int drops;              /* calls or connections lost */
	int link_errors;        /* (Psion Internet) NIFMAN's errors */
} sp_result;

/* bytes a second, from bytes and milliseconds */
unsigned long sp_rate(unsigned long aBytes, unsigned long aMs);
/* what went wrong in one half, in a few words ("" if nothing) */
void sp_why(const sp_result* r, char* aText, int aMax);
/* the verdict on the pair: 1 PASS (230400 worked, and faster), 0 FAIL;
   aText gets a short line: "1.7 x faster, no errors", "FAIL at 230400: 3
   bytes wrong"... */
int sp_verdict(const sp_result* r115, const sp_result* r230, char* aText, int aMax);
/* x.y times, as "1.7 x" (aNum against aDen; "-" if either is 0) */
void sp_times(unsigned long aNum, unsigned long aDen, char* aText, int aMax);

#ifdef __cplusplus
}
#endif
#endif
