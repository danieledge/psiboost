/* sptest.c - see sptest.h. */
#include "sptest.h"

/* (no C library: the helper on the Psion is a plain EPOC program, and
   ESTLIB is not in the 5mx's ROM) */
static void zero(void* p, int n)
{
	unsigned char* q = (unsigned char*)p;
	while (n-- > 0)
		*q++ = 0;
}

static int same(const char* a, const char* b, int n)
{
	while (n-- > 0)
		if (*a++ != *b++)
			return 0;
	return 1;
}

static int len(const char* s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* a growing line of text: strings and numbers */
typedef struct { char* p; int n, max; } line_t;

static void put(line_t* l, const char* s)
{
	while (*s && l->n < l->max - 1)
		l->p[l->n++] = *s++;
	l->p[l->n] = 0;
}

/* (with thousands commas: 1,048,576; these are all for people to read) */
static void num(line_t* l, unsigned long v)
{
	char t[16];
	int i = 0, d = 0;
	do {
		if (d && d % 3 == 0)
			t[i++] = ',';
		t[i++] = (char)('0' + v % 10); v /= 10; d++;
	} while (v && i < 15);
	while (i && l->n < l->max - 1)
		l->p[l->n++] = t[--i];
	l->p[l->n] = 0;
}

/* ---- chargen ---- */
void sp_chargen_init(sp_chargen* c)
{
	zero(c, (int)sizeof(*c));
	c->col = -2;                    /* (in step from the first whole line: whatever
	                                   came before it, the end of the modem's
	                                   CONNECT line say, is not counted) */
}

static int expect(int line, int col)
{
	if (col == 72) return '\r';
	if (col == 73) return '\n';
	return 32 + (line + col) % 95;
}

unsigned long sp_chargen_check(sp_chargen* c, const unsigned char* p, int n)
{
	unsigned long bad = 0;
	int i;
	for (i = 0; i < n; i++) {
		int b = p[i];
		if (c->synced == 0) {
			/* not in step: the first printable after a LF starts a line,
			   and its character says which */
			if (c->col == -1) {
				if (b >= 32 && b < 127) {
					c->line = b - 32;
					c->col = 1;
					c->synced = 1;
				}
				continue;
			}
			if (b == '\n')
				c->col = -1;
			continue;
		}
		if (b == expect(c->line, c->col))
			c->good++;
		else {
			c->bad++;
			bad++;
			c->synced = 0;
			c->col = b == '\n' ? -1 : -2;   /* (to the next line) */
			continue;
		}
		if (++c->col == 74) {
			c->col = 0;
			c->line = (c->line + 1) % 95;
		}
	}
	return bad;
}

/* ---- echo ---- */
static unsigned long xs(unsigned long* s)
{
	unsigned long x = *s;
	x ^= (x << 13) & 0xffffffffUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xffffffffUL;
	*s = x & 0xffffffffUL;
	return *s;
}

void sp_echo_init(sp_echo* e, unsigned long seed)
{
	zero(e, (int)sizeof(*e));
	e->tx = e->rx = seed ? seed : 0x2545F491UL;
}

void sp_echo_init_text(sp_echo* e, unsigned long seed)
{
	sp_echo_init(e, seed);
	e->text = 1;
}

static const char* const echo_set = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* the next byte of the stream (text: 64 characters then LF) */
static unsigned char echo_next(sp_echo* e, unsigned long* gen, unsigned long* pos)
{
	unsigned long v = xs(gen);
	if (!e->text)
		return (unsigned char)(v >> 11);
	if ((*pos)++ % 65 == 64)
		return '\n';
	return (unsigned char)echo_set[(v >> 11) & 63];
}

void sp_echo_fill(sp_echo* e, unsigned char* p, int n)
{
	int i;
	for (i = 0; i < n; i++)
		p[i] = echo_next(e, &e->tx, &e->txpos);
	e->sent += (unsigned long)n;
}

unsigned long sp_echo_check(sp_echo* e, const unsigned char* p, int n)
{
	unsigned long bad = 0;
	int i;
	for (i = 0; i < n; i++)
		if (p[i] != echo_next(e, &e->rx, &e->rxpos))
			bad++;
	e->back += (unsigned long)n;
	e->bad += bad;
	return bad;
}

/* ---- MD5 (RFC 1321; the ARM710T has no long multiply, and none is needed) ---- */
static const unsigned long md5_k[64] = {
	0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL, 0xf57c0fafUL, 0x4787c62aUL, 0xa8304613UL, 0xfd469501UL,
	0x698098d8UL, 0x8b44f7afUL, 0xffff5bb1UL, 0x895cd7beUL, 0x6b901122UL, 0xfd987193UL, 0xa679438eUL, 0x49b40821UL,
	0xf61e2562UL, 0xc040b340UL, 0x265e5a51UL, 0xe9b6c7aaUL, 0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL,
	0x21e1cde6UL, 0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL, 0xa9e3e905UL, 0xfcefa3f8UL, 0x676f02d9UL, 0x8d2a4c8aUL,
	0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL, 0xfde5380cUL, 0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL,
	0x289b7ec6UL, 0xeaa127faUL, 0xd4ef3085UL, 0x04881d05UL, 0xd9d4d039UL, 0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL,
	0xf4292244UL, 0x432aff97UL, 0xab9423a7UL, 0xfc93a039UL, 0x655b59c3UL, 0x8f0ccc92UL, 0xffeff47dUL, 0x85845dd1UL,
	0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL, 0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL
};
static const unsigned char md5_r[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

#define ROL(x, c) ((((x) << (c)) | ((x) >> (32 - (c)))) & 0xffffffffUL)

static void md5_block(sp_md5* m, const unsigned char* p)
{
	unsigned long w[16], a = m->a, b = m->b, c = m->c, d = m->d;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (unsigned long)p[i * 4] | ((unsigned long)p[i * 4 + 1] << 8) | ((unsigned long)p[i * 4 + 2] << 16) | ((unsigned long)p[i * 4 + 3] << 24);
	for (i = 0; i < 64; i++) {
		unsigned long f, t;
		int g;
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
		else { f = c ^ (b | (~d & 0xffffffffUL)); g = (7 * i) % 16; }
		t = d;
		d = c;
		c = b;
		b = (b + ROL((a + f + md5_k[i] + w[g]) & 0xffffffffUL, md5_r[i])) & 0xffffffffUL;
		a = t;
	}
	m->a = (m->a + a) & 0xffffffffUL;
	m->b = (m->b + b) & 0xffffffffUL;
	m->c = (m->c + c) & 0xffffffffUL;
	m->d = (m->d + d) & 0xffffffffUL;
}

void sp_md5_init(sp_md5* m)
{
	zero(m, (int)sizeof(*m));
	m->a = 0x67452301UL;
	m->b = 0xefcdab89UL;
	m->c = 0x98badcfeUL;
	m->d = 0x10325476UL;
}

void sp_md5_update(sp_md5* m, const unsigned char* p, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		m->buf[m->n++] = p[i];
		if (++m->lo == 0)
			m->hi++;
		m->lo &= 0xffffffffUL;
		if (m->n == 64) {
			md5_block(m, m->buf);
			m->n = 0;
		}
	}
}

void sp_md5_final(sp_md5* m, unsigned char out[16])
{
	unsigned long lo = m->lo, hi = m->hi, bits_lo, bits_hi;
	unsigned char pad = 0x80, z = 0;
	int i;
	bits_lo = (lo << 3) & 0xffffffffUL;
	bits_hi = ((hi << 3) | (lo >> 29)) & 0xffffffffUL;
	sp_md5_update(m, &pad, 1);
	while (m->n != 56)
		sp_md5_update(m, &z, 1);
	for (i = 0; i < 4; i++) { unsigned char b = (unsigned char)(bits_lo >> (8 * i)); sp_md5_update(m, &b, 1); }
	for (i = 0; i < 4; i++) { unsigned char b = (unsigned char)(bits_hi >> (8 * i)); sp_md5_update(m, &b, 1); }
	for (i = 0; i < 4; i++) {
		out[i] = (unsigned char)(m->a >> (8 * i));
		out[4 + i] = (unsigned char)(m->b >> (8 * i));
		out[8 + i] = (unsigned char)(m->c >> (8 * i));
		out[12 + i] = (unsigned char)(m->d >> (8 * i));
	}
}

int sp_md5_is(const unsigned char out[16], const char* aHex)
{
	static const char* hex = "0123456789abcdef";
	int i;
	for (i = 0; i < 16; i++) {
		if (!aHex[2 * i] || !aHex[2 * i + 1])
			return 0;
		if (lower(aHex[2 * i]) != hex[out[i] >> 4] || lower(aHex[2 * i + 1]) != hex[out[i] & 15])
			return 0;
	}
	return 1;
}

/* ---- HTTP ---- */
void sp_http_init(sp_http* h)
{
	zero(h, (int)sizeof(*h));
	h->length = -1;
	sp_md5_init(&h->md5);
}

/* the number after aName's ": " in the header, or -1 */
static long head_num(const char* head, int n, const char* name)
{
	int nl = len(name), i;
	for (i = 0; i + nl < n; i++) {
		int j;
		if (i > 0 && head[i - 1] != '\n')
			continue;
		for (j = 0; j < nl; j++)
			if (lower(head[i + j]) != lower(name[j]))
				break;
		if (j == nl && head[i + nl] == ':') {
			long v = 0;
			int k = i + nl + 1, any = 0;
			while (k < n && head[k] == ' ')
				k++;
			/* Content-Range: bytes 0-65535/1048576: the part's size is what
			   comes, so its two ends are taken */
			if (same(name, "Content-Range", 13)) {
				long a = -1, b = -1;
				while (k < n && head[k] != ' ') k++;
				while (k < n && head[k] == ' ') k++;
				for (; k < n && head[k] >= '0' && head[k] <= '9'; k++) a = (a < 0 ? 0 : a) * 10 + head[k] - '0';
				if (k < n && head[k] == '-') {
					k++;
					for (; k < n && head[k] >= '0' && head[k] <= '9'; k++) b = (b < 0 ? 0 : b) * 10 + head[k] - '0';
				}
				return a >= 0 && b >= a ? b - a + 1 : -1;
			}
			while (k < n && head[k] >= '0' && head[k] <= '9') {
				v = v * 10 + head[k] - '0';
				k++;
				any = 1;
			}
			return any ? v : -1;
		}
	}
	return -1;
}

int sp_http_feed(sp_http* h, const unsigned char* p, int n)
{
	int body = 0, i = 0;
	if (!h->header_done) {
		for (; i < n; i++) {
			unsigned char c = p[i];
			if (h->head_n < (int)sizeof(h->head) - 1)
				h->head[h->head_n++] = (char)c;
			if (c == '\n' && h->head_n >= 4 && same(h->head + h->head_n - 4, "\r\n\r\n", 4)) {
				h->header_done = 1;
				i++;
				break;
			}
			if (c == '\n' && h->head_n >= 2 && h->head[h->head_n - 2] == '\n') {
				h->header_done = 1;         /* (bare LF LF) */
				i++;
				break;
			}
		}
		if (!h->header_done)
			return 0;
		h->head[h->head_n] = 0;
		/* HTTP/1.x NNN */
		if (h->head_n > 12 && same(h->head, "HTTP/", 5)) {
			int k = 5;
			while (k < h->head_n && h->head[k] != ' ') k++;
			while (k < h->head_n && h->head[k] == ' ') k++;
			h->status = 0;
			while (k < h->head_n && h->head[k] >= '0' && h->head[k] <= '9')
				h->status = h->status * 10 + h->head[k++] - '0';
		}
		if (!h->status)
			h->status = -1;                 /* (not HTTP at all) */
		h->length = head_num(h->head, h->head_n, "Content-Length");
		if (h->status == 206) {
			long part = head_num(h->head, h->head_n, "Content-Range");
			if (part >= 0)
				h->length = part;
		}
	}
	body = n - i;
	if (body > 0) {
		if (h->length >= 0 && h->body + (unsigned long)body > (unsigned long)h->length)
			h->bad += h->body + (unsigned long)body - (unsigned long)h->length;
		sp_md5_update(&h->md5, p + i, body);
		h->body += (unsigned long)body;
	}
	return body;
}

int sp_http_whole(const sp_http* h)
{
	return h->header_done && h->length >= 0 && h->body == (unsigned long)h->length;
}

/* ---- the command-mode check ---- */
int sp_reply_ok(const char* aCommand, const char* aReply, int aLen)
{
	int cl = len(aCommand), i;
	int echo = 0, ok = 0;
	for (i = 0; i + cl <= aLen; i++)
		if (same(aReply + i, aCommand, cl)) {
			echo = 1;
			break;
		}
	for (i = 0; i + 2 <= aLen; i++)
		if (aReply[i] == 'O' && aReply[i + 1] == 'K')
			ok = 1;
	return echo && ok;
}

/* ---- the verdict ---- */
unsigned long sp_rate(unsigned long aBytes, unsigned long aMs)
{
	if (!aMs)
		return 0;
	/* (no 64-bit sums: bytes up to 4 GB / 1000 is fine) */
	return aBytes / aMs * 1000UL + (aBytes % aMs) * 1000UL / aMs;
}

void sp_times(unsigned long aNum, unsigned long aDen, char* aText, int aMax)
{
	line_t t;
	t.p = aText; t.n = 0; t.max = aMax; aText[0] = 0;
	if (!aNum || !aDen) {
		put(&t, "-");
		return;
	}
	num(&t, aNum / aDen); put(&t, "."); num(&t, aNum % aDen * 10 / aDen); put(&t, " x");
}

void sp_why(const sp_result* r, char* aText, int aMax)
{
	line_t why;
	why.p = aText; why.n = 0; why.max = aMax; aText[0] = 0;
	if (!r->ran)
		put(&why, "not reached");
	else if (!r->divider_ok) {
		put(&why, "the port was not at "); num(&why, (unsigned long)r->baud); put(&why, " bps");
	} else if (r->cmds_bad) {
		num(&why, (unsigned long)r->cmds_bad); put(&why, " of "); num(&why, (unsigned long)r->cmds);
		put(&why, " modem commands garbled");
	} else if (r->down_bad || r->echo_bad) {
		num(&why, r->down_bad + r->echo_bad); put(&why, r->down_bad + r->echo_bad == 1 ? " byte wrong" : " bytes wrong");
	} else if (r->drops) {
		put(&why, "the connection dropped or failed "); num(&why, (unsigned long)r->drops);
		put(&why, r->drops == 1 ? " time" : " times");
	} else if (r->link_errors) {
		num(&why, (unsigned long)r->link_errors); put(&why, r->link_errors == 1 ? " link error" : " link errors");
	} else if (r->stalls) {
		num(&why, (unsigned long)r->stalls); put(&why, r->stalls == 1 ? " stall" : " stalls"); put(&why, " of 3 s or more");
	} else if (!r->down_bytes || !r->echo_bytes)
		put(&why, "nothing came through");
}

int sp_verdict(const sp_result* r115, const sp_result* r230, char* aText, int aMax)
{
	unsigned long d115 = sp_rate(r115->down_bytes, r115->down_ms);
	unsigned long d230 = sp_rate(r230->down_bytes, r230->down_ms);
	char why[100], x[16];
	line_t t;
	t.p = aText; t.n = 0; t.max = aMax; aText[0] = 0;
	sp_why(r115, why, (int)sizeof(why));
	if (why[0]) {
		put(&t, "FAIL at 115,200 bps: "); put(&t, why);
		return 0;
	}
	sp_why(r230, why, (int)sizeof(why));
	if (why[0]) {
		put(&t, "FAIL at 230,400 bps: "); put(&t, why);
		return 0;
	}
	sp_times(d230, d115, x, (int)sizeof(x));
	/* faster by a quarter at least, or 230400 is not doing its job */
	if (d230 * 4 < d115 * 5) {
		put(&t, "FAIL: no faster at 230,400 bps ("); put(&t, x); put(&t, " the 115,200 bps rate)");
		return 0;
	}
	put(&t, x); put(&t, " faster, no errors");
	return 1;
}
