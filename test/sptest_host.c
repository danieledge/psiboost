/* sptest_host.c - PsiBoost's link test logic (sptest.c) on a PC, against
 * server/stdsvc.py: a real chargen and echo leg over TCP, a corrupted
 * copy of each that must be caught, the HTTP response parser, MD5 against
 * RFC 1321's test vectors, the command-mode check and the verdict.
 *   make -C test        (starts stdsvc.py on 127.0.0.1, base port 17100)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include "../sptest.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int dial(int port)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
		perror("connect");
		exit(1);
	}
	return s;
}

static void md5_hex(const char* s, char* hex)
{
	sp_md5 m;
	unsigned char out[16];
	int i;
	sp_md5_init(&m);
	sp_md5_update(&m, (const unsigned char*)s, (int)strlen(s));
	sp_md5_final(&m, out);
	for (i = 0; i < 16; i++)
		sprintf(hex + 2 * i, "%02x", out[i]);
}

int main(int argc, char** argv)
{
	int base = argc > 1 ? atoi(argv[1]) : 17100;
	unsigned char buf[4096];
	sp_result r;
	char text[200];
	memset(&r, 0, sizeof(r));
	r.baud = 115200;
	r.divider_ok = 1;
	r.ran = 1;

	/* chargen, 1 s, every byte right */
	{
		sp_chargen c;
		int s = dial(base + 19), n;
		long t0 = now_ms();
		sp_chargen_init(&c);
		while (now_ms() - t0 < 1000 && (n = (int)read(s, buf, sizeof(buf))) > 0) {
			r.down_bad += sp_chargen_check(&c, buf, n);
			r.down_bytes += (unsigned long)n;
		}
		r.down_ms = (unsigned long)(now_ms() - t0);
		close(s);
		printf("chargen: %lu bytes in %lu ms, %lu wrong\n", r.down_bytes, r.down_ms, r.down_bad);
		CHECK(r.down_bytes > 100000 && r.down_bad == 0 && c.good + 80 >= r.down_bytes);
		/* the same stream with bytes damaged: caught, and in step again after */
		{
			sp_chargen d;
			int i;
			unsigned char* copy = malloc(74 * 300);
			for (i = 0; i < 74 * 300; i++) {
				int line = i / 74, col = i % 74;
				copy[i] = (unsigned char)(col == 72 ? '\r' : col == 73 ? '\n' : 32 + (line + col) % 95);
			}
			copy[1000] ^= 1;
			copy[5000] = '\n';
			memmove(copy + 9000, copy + 9001, 74 * 300 - 9001);   /* (a byte lost) */
			sp_chargen_init(&d);
			CHECK(sp_chargen_check(&d, copy, 74 * 300 - 1) >= 3);
			CHECK(d.synced == 1 && d.good > 74 * 290);
			free(copy);
		}
	}
	/* echo, 1 s, both ways at once */
	{
		sp_echo e;
		int s = dial(base + 7);
		long t0 = now_ms();
		sp_echo_init(&e, 12345);
		while (now_ms() - t0 < 1000) {
			fd_set rd, wr;
			struct timeval tv = { 0, 50000 };
			FD_ZERO(&rd);
			FD_ZERO(&wr);
			FD_SET(s, &rd);
			if (e.sent - e.back < 16384)
				FD_SET(s, &wr);
			if (select(s + 1, &rd, &wr, 0, &tv) <= 0)
				continue;
			if (FD_ISSET(s, &wr)) {
				sp_echo_fill(&e, buf, 512);
				if (write(s, buf, 512) != 512)
					break;
			}
			if (FD_ISSET(s, &rd)) {
				int n = (int)read(s, buf, sizeof(buf));
				if (n <= 0)
					break;
				sp_echo_check(&e, buf, n);
			}
		}
		r.echo_ms = (unsigned long)(now_ms() - t0);
		r.echo_bytes = e.back;
		r.echo_bad = e.bad;
		close(s);
		printf("echo: %lu sent, %lu back, %lu wrong\n", e.sent, e.back, e.bad);
		CHECK(e.back > 100000 && e.bad == 0);
		{
			sp_echo f;
			unsigned char p[300];
			sp_echo_init(&f, 7);
			sp_echo_fill(&f, p, 300);
			p[17] ^= 0x80;
			CHECK(sp_echo_check(&f, p, 300) == 1);
		}
		/* the text stream: 7-bit, no CR, LF every 65th byte, checked the same way */
		{
			sp_echo f;
			unsigned char p[400];
			int i, lf = 0;
			sp_echo_init_text(&f, 9);
			sp_echo_fill(&f, p, 400);
			for (i = 0; i < 400; i++) {
				CHECK(p[i] < 128 && p[i] != '\r');
				if (p[i] == '\n') { lf++; CHECK(i % 65 == 64); }
			}
			CHECK(lf == 6);
			CHECK(sp_echo_check(&f, p, 200) == 0);
			p[250] = 'x' == p[250] ? 'y' : 'x';
			CHECK(sp_echo_check(&f, p + 200, 200) == 1);
		}
	}
	/* MD5: RFC 1321's vectors, and in pieces */
	{
		char hex[33];
		md5_hex("", hex);
		CHECK(strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e") == 0);
		md5_hex("abc", hex);
		CHECK(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);
		md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890", hex);
		CHECK(strcmp(hex, "57edf4a22be3c955ac49da2e2107b67a") == 0);
		{
			sp_md5 m;
			unsigned char out[16], big[100000];
			int i;
			for (i = 0; i < (int)sizeof(big); i++)
				big[i] = (unsigned char)(i * 7 + (i >> 8));
			sp_md5_init(&m);
			for (i = 0; i < (int)sizeof(big); i += 333)
				sp_md5_update(&m, big + i, i + 333 <= (int)sizeof(big) ? 333 : (int)sizeof(big) - i);
			sp_md5_final(&m, out);
			/* (against the whole at once) */
			{
				sp_md5 w;
				unsigned char o2[16];
				sp_md5_init(&w);
				sp_md5_update(&w, big, (int)sizeof(big));
				sp_md5_final(&w, o2);
				CHECK(memcmp(out, o2, 16) == 0);
			}
			CHECK(sp_md5_is(out, "00000000000000000000000000000000") == 0);
		}
		{
			unsigned char out[16] = { 0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72 };
			CHECK(sp_md5_is(out, "900150983cd24fb0d6963f7d28e17f72"));
			CHECK(sp_md5_is(out, "900150983CD24FB0D6963F7D28E17F72"));
			CHECK(!sp_md5_is(out, "900150983cd24fb0d6963f7d28e17f7"));
		}
	}
	/* HTTP: a response in pieces, Content-Length, 206 with Content-Range, too much body */
	{
		sp_http h;
		const char* resp = "HTTP/1.1 200 OK\r\nServer: x\r\nContent-Length: 10\r\nConnection: close\r\n\r\n0123456789";
		int n = (int)strlen(resp), i, body = 0;
		sp_http_init(&h);
		for (i = 0; i < n; i += 7)
			body += sp_http_feed(&h, (const unsigned char*)resp + i, i + 7 <= n ? 7 : n - i);
		CHECK(h.status == 200 && h.length == 10 && body == 10 && h.body == 10 && h.bad == 0 && sp_http_whole(&h));
		{
			unsigned char out[16];
			sp_md5_final(&h.md5, out);
			CHECK(sp_md5_is(out, "781e5e245d69b566979b86e28d23f2c7"));   /* md5("0123456789") */
		}
		sp_http_init(&h);
		resp = "HTTP/1.0 206 Partial Content\r\ncontent-range: bytes 0-65535/1048576\r\n\r\nabc";
		sp_http_feed(&h, (const unsigned char*)resp, (int)strlen(resp));
		CHECK(h.status == 206 && h.length == 65536 && h.body == 3 && !sp_http_whole(&h));
		sp_http_init(&h);
		resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nabcd";
		sp_http_feed(&h, (const unsigned char*)resp, (int)strlen(resp));
		CHECK(h.status == 200 && h.body == 4 && h.bad == 2 && !sp_http_whole(&h));
		sp_http_init(&h);
		resp = "HTTP/1.1 404 Not Found\r\n\r\nnope";
		sp_http_feed(&h, (const unsigned char*)resp, (int)strlen(resp));
		CHECK(h.status == 404 && h.length == -1 && h.body == 4);
		sp_http_init(&h);
		resp = "NO CARRIER\r\n\r\n";
		sp_http_feed(&h, (const unsigned char*)resp, (int)strlen(resp));
		CHECK(h.status == -1);
	}
	/* the command-mode check */
	{
		const char* good = "ATE1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0\r\r\nOK\r\n";
		const char* bad = "ATE1V1Q0E1V1Q\xf3\x0eV1Q0E1V1Q0E1V1Q0E1V1Q0\r\r\nOK\r\n";
		CHECK(sp_reply_ok(SP_LONG_LINE, good, (int)strlen(good)));
		CHECK(!sp_reply_ok(SP_LONG_LINE, bad, (int)strlen(bad)));
		CHECK(!sp_reply_ok("AT", "\xf0\xf0", 2));
	}
	/* the verdict on a pair */
	{
		sp_result f = r;
		char x[16];
		f.baud = 230400;
		f.down_bytes = r.down_bytes * 2;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 1);
		printf("%s\n", text);
		CHECK(strstr(text, "2.0 x faster, no errors") != 0);
		f.down_bytes = r.down_bytes;                     /* secretly still 115200 */
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0);
		printf("%s\n", text);
		CHECK(strstr(text, "no faster") != 0);
		f.down_bytes = r.down_bytes * 2; f.down_bad = 3;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0 && strstr(text, "FAIL at 230,400 bps: 3 bytes wrong") != 0);
		f.down_bad = 0; f.stalls = 1;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0);
		f.stalls = 0; f.divider_ok = 0;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0);
		printf("%s\n", text);
		f.divider_ok = 1; f.cmds = 20; f.cmds_bad = 2;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0);
		f.cmds_bad = 0; f.route = 1; f.link_errors = 1;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0);
		printf("%s\n", text);
		f.link_errors = 0; f.ran = 0;
		CHECK(sp_verdict(&r, &f, text, sizeof(text)) == 0 && strstr(text, "not reached") != 0);
		{
			sp_result g = r;
			g.drops = 1;
			CHECK(sp_verdict(&g, &f, text, sizeof(text)) == 0 && strstr(text, "FAIL at 115,200 bps") != 0);
		}
		sp_times(17000, 10000, x, sizeof(x));
		CHECK(strcmp(x, "1.7 x") == 0);
		sp_times(0, 10000, x, sizeof(x));
		CHECK(strcmp(x, "-") == 0);
	}
	printf("sptest_host: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
