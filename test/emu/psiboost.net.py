# PsiBoost's emulator tests, loaded by core/tools/emu/net.py from the
# checkout's test/emu/. They run with net.py's names (Script, make_card,
# emulate, grep, ENTER, WORK, ctx.modem); asked for by name:
#   python3 core/tools/emu/net.py boostsetup boost
# The modem is psiconnect's hosttest/hostmodem (a checkout beside this one,
# or PSICONNECT=DIR). The bridge has no baud rate and the emulator does not
# model the UART's divider (it reads 0), so the speed checks are skipped and
# the speeds are nominal here: what the port does at 230400 is the 5mx's
# alone. The hook going in and out is real.


# The link test on the modem route against hostmodem and server/stdsvc.py
# (7107 echo, 7119 chargen): the card sets it up (psiboosttest: your own
# server 127.0.0.1, quick), the script opens Tools > Test the speed..., OK.
# The test runs its 115200 half (the modem found with AT, the command-mode
# check, chargen and echo dialled, +++ ATH), then tells the modem 230400
# (AT$SB=230400, which hostmodem takes), puts the hook in and opens the
# port again for the second half, and at the end puts the modem back at
# 115200 and the hook out, as 230400 always is off on this card. Needs
# server/stdsvc.py listening on 127.0.0.1 (base 7100):
#   python3 server/stdsvc.py --host 127.0.0.1 &
#   python3 core/tools/emu/net.py boost
def test_boost(ctx):
    # (the emulator's clock runs ahead of the PC's, by an amount that varies
    # with the load: the helper's 1.1 s guard times round +++ arrive short,
    # so hostmodem's guard is made 20 ms, where its build has HOSTMODEM_S12)
    os.environ['HOSTMODEM_S12'] = '1'
    try:
        log = ctx.modem('boost')
    finally:
        del os.environ['HOSTMODEM_S12']
    card = os.path.join(WORK, 'boost.img')
    make_card('psiboosttest', card, 'EMU_PKG_BOOST')
    # (PsiBoost is open by 46 s: Menu, Right three times to Tools, Enter on
    # its first command, Test the speed..., then OK)
    s = Script(62).key(148).key(15).key(15).key(15).key(ENTER)
    s.wait(3).key(ENTER)
    out = emulate('boost', card, ctx.sock, s, secs=360)
    ok = (grep(log, 'ATDT127.0.0.1:7119') and grep(log, 'ATDT127.0.0.1:7107') and grep(log, '[cmd: H]')
          and grep(log, '[cmd: $SB=230400]', '[cmd: $SB=115200]'))
    return ok, out


TESTS['boost'] = test_boost


# 230400 always against hostmodem: the page, Enter on "230400 always" (the
# first choice): the modem found at 115200, AT$CS=0, AT$SB=230400, the hook
# in, the port opened again, AT, AT&W; then Enter again to turn it off:
# AT$SB=115200, the hook out, AT, AT&W.
#   python3 core/tools/emu/net.py boostsetup
def test_boostsetup(ctx):
    log = ctx.modem('boostsetup')
    card = os.path.join(WORK, 'boostsetup.img')
    make_card('psiboost', card, 'EMU_PKG_BOOST')
    # (PsiBoost is open by 46 s here: a tap there would land on the choice)
    s = Script(62).key(ENTER).wait(40).key(ENTER)
    out = emulate('boostsetup', card, ctx.sock, s, secs=40)
    # (hostmodem logs each command as "[cmd: $SB=230400]")
    ok = grep(log, '[cmd: $SB=230400]', '[cmd: &W]', '[cmd: $SB=115200]')
    return ok, out


TESTS['boostsetup'] = test_boostsetup
