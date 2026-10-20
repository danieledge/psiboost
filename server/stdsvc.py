#!/usr/bin/env python3
"""stdsvc.py - the three classic TCP test services, for PsiBoost's link test
(any modem dials them with a plain ATDT host:port; Psion Internet reaches
them with an ordinary socket):

    echo     (RFC 862)  port 7107: every byte sent comes back
    discard  (RFC 863)  port 7109: every byte sent is thrown away
    chargen  (RFC 864)  port 7119: lines of 72 printable characters, each
                                   starting one character further on, CR LF

On 7100 and up (--base) because an unprivileged user cannot listen below
1024; with --base 0 they are on the standard ports 7, 9 and 19 (as root).
Run it on a computer the Psion can reach, and give PsiBoost its name or
address and the base port (Tools > Test the speed, Your own server: Server
name or address, Base port). A connection is
closed after 20 minutes, and at most 8 are served at once.

    python3 stdsvc.py [--host 0.0.0.0] [--base 7100]
"""
import argparse, asyncio, time

PRINTABLE = bytes(range(32, 127))       # (95 characters)
MAX_SECS = 20 * 60
MAX_CONN = 8
live = 0


def chargen_line(i):
    k = i % 95
    rot = PRINTABLE[k:] + PRINTABLE[:k]
    return rot[:72] + b"\r\n"


async def serve(kind, reader, writer):
    global live
    if live >= MAX_CONN:
        writer.close()
        return
    live += 1
    peer = writer.get_extra_info("peername")
    t0 = time.monotonic()
    n_in = n_out = 0
    try:
        if kind == "chargen":
            i = 0
            block = b"".join(chargen_line(j) for j in range(95))      # (95 lines repeat)
            while time.monotonic() - t0 < MAX_SECS:
                writer.write(block)
                n_out += len(block)
                await writer.drain()
        else:
            while time.monotonic() - t0 < MAX_SECS:
                d = await asyncio.wait_for(reader.read(4096), timeout=120)
                if not d:
                    break
                n_in += len(d)
                if kind == "echo":
                    writer.write(d)
                    n_out += len(d)
                    await writer.drain()
    except (asyncio.TimeoutError, ConnectionError, OSError):
        pass
    finally:
        live -= 1
        try:
            writer.close()
        except OSError:
            pass
        print("%s %s: %d in, %d out, %.1f s" % (kind, peer, n_in, n_out, time.monotonic() - t0), flush=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--base", type=int, default=7100)
    a = ap.parse_args()
    servers = []
    for kind, off in (("echo", 7), ("discard", 9), ("chargen", 19)):
        s = await asyncio.start_server(lambda r, w, k=kind: serve(k, r, w), a.host, a.base + off)
        servers.append(s)
        print("%s on %s:%d" % (kind, a.host, a.base + off), flush=True)
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    asyncio.run(main())
