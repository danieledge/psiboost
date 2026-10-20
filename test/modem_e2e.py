#!/usr/bin/env python3
"""modem_e2e.py - PsiBoost's modem-route link test, done the way the helper
does it, against a real modem's code on a PC (PsiConnect's
hosttest/hostmodem, over its unix socket) and server/stdsvc.py: the
command-mode check (AT, ATI, the long echoed line), then calls to chargen
and echo with ATDT host:port, every byte checked, and +++ ATH between them.
Only standard modem behaviour, nothing one modem alone does.
    python3 modem_e2e.py HOSTMODEM [BASEPORT]
"""
import os, socket, subprocess, sys, tempfile, time

LONG = b"ATE1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0E1V1Q0"
checks = fails = 0


def check(c, what):
    global checks, fails
    checks += 1
    if not c:
        fails += 1
        print("FAIL", what)


def talk(s, cmd, wait=1.0):
    s.sendall(cmd + b"\r")
    got, t0 = b"", time.time()
    while time.time() - t0 < wait and b"OK\r\n" not in got and b"ERROR" not in got and b"CONNECT" not in got:
        try:
            got += s.recv(4096)
        except socket.timeout:
            pass
    return got


def drain(s, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            s.recv(65536)
        except socket.timeout:
            pass


def hangup(s):
    # (the guard times are quiet from the Psion only: it goes on reading)
    drain(s, 1.1)
    s.sendall(b"+++")
    drain(s, 1.1)
    got = talk(s, b"ATH")
    return got


def chargen_ok(data):
    p = bytes(range(32, 127))
    start = data.index(b"\n") + 1                # (from the first whole line)
    line = data[start] - 32
    bad, i = 0, start
    while i + 74 <= len(data):
        want = (p[line:] + p[:line])[:72] + b"\r\n" if line else p[:72] + b"\r\n"
        if data[i:i + 74] != want:
            bad += 1
        i += 74
        line = (line + 1) % 95
    return bad, (i - start) // 74


def main():
    hm = sys.argv[1]
    base = int(sys.argv[2]) if len(sys.argv) > 2 else 17200
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "m.sock")
    svc = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(__file__), "../server/stdsvc.py"),
                            "--host", "127.0.0.1", "--base", str(base)], stdout=subprocess.DEVNULL)
    proc = subprocess.Popen([hm, sock], env=dict(os.environ, HOSTMODEM_JOIN_MS="10"),
                            stdout=open(os.path.join(tmp, "hm.log"), "w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        s.settimeout(0.2)
        # the command-mode check: echo on, AT and ATI, the long line, 20 times
        check(b"OK" in talk(s, b"ATE1"), "ATE1")
        bad = 0
        for i in range(20):
            g = talk(s, b"AT")
            g2 = talk(s, LONG)
            if not (b"OK" in g and LONG in g2 and b"OK" in g2):
                bad += 1
        check(bad == 0, "the command-mode check: %d of 20 bad" % bad)
        check(b"OK" in talk(s, b"ATNET0"), "ATNET0")
        # chargen: 2 s of it, every line right
        g = talk(s, b"ATDT127.0.0.1:%d" % (base + 19), 5)
        check(b"CONNECT" in g, "chargen: CONNECT (%r)" % g[-40:])
        data, t0 = b"", time.time()
        while time.time() - t0 < 2:
            try:
                data += s.recv(65536)
            except socket.timeout:
                pass
        nbad, lines = chargen_ok(data)
        print("chargen through the modem: %d bytes, %d lines, %d bad" % (len(data), lines, nbad))
        check(lines > 100 and nbad == 0, "chargen: every line right")
        check(b"OK" in hangup(s), "chargen: +++ ATH")
        # echo: every byte value, 0xFF and "+++" included, 1 s
        try:
            s.recv(65536)
        except socket.timeout:
            pass
        g = talk(s, b"ATDT127.0.0.1:%d" % (base + 7), 5)
        check(b"CONNECT" in g, "echo: CONNECT")
        time.sleep(0.2)
        payload = bytes((i * 7 + 3) & 0xff for i in range(20000)) + b"+++" + bytes(range(256))
        s.sendall(payload)
        back, t0 = b"", time.time()
        while len(back) < len(payload) and time.time() - t0 < 5:
            try:
                back += s.recv(65536)
            except socket.timeout:
                pass
        print("echo through the modem: %d sent, %d back" % (len(payload), len(back)))
        check(back == payload, "echo: every byte back as sent")
        check(b"OK" in hangup(s), "echo: +++ ATH")
        check(b"OK" in talk(s, b"AT"), "a command after the calls")
    finally:
        proc.terminate()
        svc.terminate()
    print("modem_e2e: %d checks, %d failed" % (checks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
