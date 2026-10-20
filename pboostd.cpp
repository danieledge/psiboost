// pboostd.cpp - PsiBoost's helper (pboostd.exe): the serial work for
// 230400 always, Free the serial port, the quick look at the modem, the
// link test and Send log. Started by PsiBoost.app; ends soon after it
// closes. See spshared.h and docs/how-it-works.md.
//
// 230400 always: the hook (PBHOOK20.LDD, pbhook.h) makes every program that
// asks the RS-232 port for 115200 get 230400, from the first byte. Turning
// it on: the port free, the modem found at either speed (FindModem), told
// AT$SB=230400 (or ATB230400, AT+IPR=230400 on other modems) at 115200 - a
// modem that takes AT$SB changes once its OK has gone, and goes back by
// itself in 15 s if no command is heard at the new speed - then the port
// closed, the hook put
// in, the port opened again (now really 230400), AT checked, and AT&W to
// save. Anything failing: the hook out, the modem back at 115200 at
// whatever speed answers, saved, and the step that failed said. Turning it
// off is the same the other way: AT$SB=115200 at 230400, the hook out, AT
// at 115200, AT&W. MoveTo does both.
//
// Whenever the helper talks to the modem it first finds the speed the modem
// answers AT at (FindModem). Every change of the Psion's divider by the
// UART driver (PBUART21.LDD) is read back (SetDivider) and forgotten when
// the helper's port closes (TSpSerial::Close), every command and reply goes
// to the log, and a step that fails says which step it was and what the
// modem said (Step).
#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <f32file.h>
#include <c32comm.h>
#include <es_sock.h>
#include <in_sock.h>
#include <nifman.h>
#include <netdial.h>
#include "pbuart.h"
#include "psilink.h"                       // (psion-core: Connection settings)
#include "spshared.h"
#include "pbhook.h"
extern "C" {
#include "sptest.h"
}

const TInt KTickUs = 31250;                  // 32 looks a second at the UART while it is on
const TInt KIdleTickUs = 125000;             // 8 while it is off
const TInt KUartEnable = 0x01;               // UART2 CON: UARTEN
const TInt KDiv115200 = 3, KDiv230400 = 1;
const TInt KPortWaitSecs = 20;               // Psion Internet letting COMM::0 go after a link goes down (as psiglue)
const TInt KStageLinkUp = 1000;              // EIfProgressLinkUp: the 5mx may say this rather than EConnectionOpen (psiglue)
const TInt KAppGoneMs = 5000;                // no beat from PsiBoost for this long: it has gone
_LIT(KLogName, "PsiBoost.log");
_LIT8(KSpVersion8, "0.23");                  // (as psiboost.cpp's)
// The public servers (Tools > Test the speed, "Public (internet)"), checked
// on 19 Oct 2026: a 1 MB file over plain HTTP (port 80, no
// redirect, Content-Length, Range honoured), the second tried if the first
// cannot be reached; and a TCP echo that passes text (tcpbin.com drops CR
// and some other bytes, and closes after 15 s of quiet, so the echo leg
// sends 7-bit lines without a pause). Both routes reach them through the
// modem.
struct TSpPublicFile { const TText8* iHost; const TText8* iPath; TInt iSize; const char* iMd5; };
static const TSpPublicFile KPublicFiles[2] =
	{
	// random bytes (a real test of the line); the checksum is the file's as measured, not published
	{ (const TText8*)"ipv4.download.thinkbroadband.com", (const TText8*)"/1MB.zip", 1048576, "f00be31bb73fe783209497d80aa1de89" },
	// all zero bytes; the checksum is published at /md5sum.txt
	{ (const TText8*)"speedtest.tele2.net", (const TText8*)"/1MB.zip", 1048576, "b6d81b360a5672d80c27430f39153e2c" }
	};
_LIT8(KPublicEchoHost, "tcpbin.com");
const TInt KPublicEchoPort = 4242;
static TFileName gLogFile;

static TSpShared* gS = 0;
static RPbUart gKern;                        // the UART's registers (PBUART21.LDD)
static TBool gKernOpen = EFalse;
static RPbHook gHook;                        // the hook
static TBool gHookOpen = EFalse;
static RFs* gFs = 0;
static TBuf8<120> gLastStep;                 // the last Step: added to a verdict that failed before any data
static TBool gTalkLog = ETrue;               // Talk writes each command and reply to the log
static TInt gModemSpeed = -1;                // where the modem last answered AT: 115200, 230400, 0 nowhere, -1 not looked
static TInt gSelf = 0;                       // the helper has the serial port open itself

static TUint Ms(TUint aT0) { return (TUint)((User::TickCount() - aT0) * 15625 / 1000); }

// ----- the log: PsiBoost.log beside the program, lines with the time ----------
static void Log(const TDesC8& aLine)
	{
	if (!gFs)
		return;
	RFile f;
	TInt r = f.Open(*gFs, gLogFile, EFileWrite | EFileShareAny);
	if (r == KErrNotFound)
		r = f.Create(*gFs, gLogFile, EFileWrite | EFileShareAny);
	if (r != KErrNone)
		return;
	TInt size = 0;
	f.Size(size);
	if (size > 32768)                        // (kept small: when full it becomes PsiBoost.old)
		{
		f.Close();
		TFileName old(gLogFile);
		old.SetLength(old.Length() - 3);
		old.Append(_L("old"));
		gFs->Delete(old);
		if (gFs->Rename(gLogFile, old) != KErrNone || f.Create(*gFs, gLogFile, EFileWrite | EFileShareAny) != KErrNone)
			{
			if (f.Open(*gFs, gLogFile, EFileWrite | EFileShareAny) != KErrNone)
				return;
			f.SetSize(0);
			}
		}
	TInt end = 0;
	f.Seek(ESeekEnd, end);
	TTime t;
	t.HomeTime();
	TDateTime d = t.DateTime();
	TBuf8<200> l;
	l.Format(_L8("%02d:%02d:%02d "), d.Hour(), d.Minute(), d.Second());
	l.Append(aLine.Left(Min(aLine.Length(), 180)));
	l.Append(_L8("\r\n"));
	f.Write(l);
	f.Close();
	}

// a number with thousands commas, then aUnit if given: "230,400 bps"
// (EPOC's Format has no grouping), for %S
class TNum8 : public TBuf8<24>
	{
public:
	TNum8(TUint aV, const char* aUnit = 0)
		{
		TBuf8<12> d;
		d.Num(aV);
		for (TInt i = 0; i < d.Length(); i++)
			{
			if (i > 0 && (d.Length() - i) % 3 == 0)
				Append(',');
			Append(d[i]);
			}
		if (aUnit)
			Append(TPtrC8((const TUint8*)aUnit));
		}
	};

// control characters as dots, for the log
static void Printable(const TDesC8& aIn, TDes8& aOut)
	{
	aOut.Zero();
	for (TInt i = 0; i < aIn.Length() && aOut.Length() < aOut.MaxLength(); i++)
		{
		TUint8 c = aIn[i];
		aOut.Append(c >= 32 && c < 127 ? (TChar)c : (TChar)'.');
		}
	}

// the page's last line
static void Say(const TDesC8& aMsg)
	{
	gS->iMsg.Copy(aMsg.Left(Min(aMsg.Length(), gS->iMsg.MaxLength())));
	gS->iHelperSeq++;
	Log(aMsg);
	}

// the test's progress (the test choice on the page)
static void TestNow(const TDesC8& aMsg)
	{
	gS->iTestNow.Copy(aMsg.Left(Min(aMsg.Length(), gS->iTestNow.MaxLength())));
	gS->iHelperSeq++;
	Log(aMsg);
	}

// a step's outcome: the log, the page's last line, the test's progress while
// a test runs, and remembered for the verdict
static void Step(const TDesC8& aMsg)
	{
	gS->iMsg.Copy(aMsg.Left(Min(aMsg.Length(), gS->iMsg.MaxLength())));
	if (gS->iTestState == ESpTestRunning)
		gS->iTestNow.Copy(aMsg.Left(Min(aMsg.Length(), gS->iTestNow.MaxLength())));
	gLastStep.Copy(aMsg.Left(Min(aMsg.Length(), gLastStep.MaxLength())));
	gS->iHelperSeq++;
	Log(aMsg);
	}

// ----- the UART -----------------------------------------------------------------
static TBool UartOn()
	{
	TInt c = gKern.Read8(EPuRegCon);
	return c >= 0 && (c & KUartEnable);
	}

// (the emulator does not model UBRCR and reads 0 or 0xff: not known, so
// the checks of the speed are skipped there)
static TInt Divider()
	{
	TUint32 v = 0;
	if (gKern.Read32(EPuRegUbrcr, v) != KErrNone || (v & 0xff) == 0xff || (v & 0xff) == 0)
		return -1;
	return (TInt)(v & 0xff);
	}

// the Psion's open port to 230400 (divider 1) or 115200 (3) by the UART
// driver, and read back: only while the UART is on, and only
// for the helper's own port. KErrNone only if the register now holds the
// value asked for. Logged unless aQuiet and done.
static TInt SetDivider(TInt aDiv, TBool aQuiet = EFalse)
	{
	TInt r = gKern.UbrcrSet(aDiv);
	User::After(20000);
	TInt now = Divider();
	TBool ok = r == KErrNone && now == aDiv;
	if (!ok || !aQuiet)
		{
		TBuf8<120> m;
		TNum8 rate(aDiv == KDiv230400 ? 230400 : 115200, " bps");
		m.Format(_L8("The Psion's port to %S: %s (driver %d, divider now %d, UART %s)"), &rate,
			ok ? "done" : "NOT done", r, now, UartOn() ? "on" : "off");
		Log(m);
		}
	return ok ? KErrNone : (r != KErrNone ? r : KErrGeneral);
	}

// ----- the hook -------------------------------------------------------------------
// the hook as it is now (TPbFound); aFactory the RS-232 port's factory
static TInt HookFound(TUint32& aFactory)
	{
	aFactory = 0;
	if (!gHookOpen)
		return EPbNoFactory;
	return PbFindFactory(gHook, aFactory);
	}

static TBool HookOn()
	{
	TUint32 f;
	return HookFound(f) == EPbHooked;
	}

// The hook put in (aOn) or taken out, for ports opened from now on (one
// already open keeps what it had). KErrNone, or why not (said): KErrInUse
// when another driver has hooked the port.
static TInt HookSet(TBool aOn)
	{
	if (!gHookOpen)
		{
		Step(_L8("The hook's driver is not loaded"));
		return KErrNotReady;
		}
	TUint32 f = 0;
	TInt found = PbFindFactory(gHook, f);
	TBuf8<140> m;
	if (found == EPbForeign)
		{
		Step(_L8("The serial port is hooked by another driver: restart the Psion"));
		return KErrInUse;
		}
	if (found == EPbNoFactory)
		{
		Step(_L8("The serial port's driver (Comm.Wind2) was not found: the hook cannot go in"));
		return KErrNotFound;
		}
	if (aOn == (found == EPbHooked))
		return KErrNone;
	TInt r = aOn ? gHook.Install(f) : gHook.Remove(f);
	if (r != KErrNone)
		{
		m.Format(aOn ? _L8("The hook was not put in (%d, factory %08x): nothing was written")
		             : _L8("The hook was not taken out (%d, factory %08x)"), r, f);
		Step(m);
		return r;
		}
	m.Format(aOn ? _L8("The hook is in (factory %08x): a port opened at 115,200 bps now gets 230,400 bps")
	             : _L8("The hook is out (factory %08x): a port gets the speed it asks for"), f);
	Log(m);
	gS->iHelperSeq++;
	return KErrNone;
	}

// ----- the serial port, opened by the helper itself -------------------------
class TSpSerial
	{
public:
	TSpSerial() : iServerOpen(EFalse), iOpen(EFalse), iTimerOpen(EFalse) {}
	~TSpSerial() { Close(); }
	TInt Open();
	TBool IsOpen() const { return iOpen; }
	TInt Write(const TDesC8& aData);
	// what came within aMs (at least one byte, unless nothing came): KErrNone,
	// KErrTimedOut, or the port's error
	TInt Read(TDes8& aBuf, TInt aMs);
	TInt Talk(const TDesC8& aCommand, TInt aTimeoutMs, TDes8* aReply = 0);
	void Drain(TInt aMs);
	void HangUp();
	void Close();
private:
	RCommServ iServer;
	RComm iComm;
	RTimer iTimer;
	TBool iServerOpen, iOpen, iTimerOpen;
	};

// at 115200, as every program opens it (230400 with the hook in)
TInt TSpSerial::Open()
	{
	TInt r = User::LoadPhysicalDevice(_L("EUART1"));
	if (r == KErrNone || r == KErrAlreadyExists)
		r = User::LoadLogicalDevice(_L("ECOMM"));
	if (r == KErrNone || r == KErrAlreadyExists)
		r = StartC32();
	if (r == KErrNone || r == KErrAlreadyExists)
		{
		r = iServer.Connect();
		if (r == KErrNone)
			{
			iServerOpen = ETrue;
			r = iServer.LoadCommModule(_L("ECUART"));
			}
		}
	if (r == KErrNone || r == KErrAlreadyExists)
		r = iComm.Open(iServer, _L("COMM::0"), ECommExclusive);
	if (r != KErrNone)
		{
		Close();
		return r;
		}
	iOpen = ETrue;
	gSelf++;
	if (!iTimerOpen)
		iTimerOpen = iTimer.CreateLocal() == KErrNone;
	TCommConfig cfg;
	iComm.Config(cfg);
	cfg().iRate = EBps115200;
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iFifo = EFifoEnable;
	cfg().iTerminatorCount = 0;
	cfg().iHandshake = 0;
	r = iComm.SetConfig(cfg);
	if (r != KErrNone)
		{
		Close();
		return r;
		}
	iComm.SetReceiveBufferLength(16384);
	iComm.SetSignals(KSignalDTR | KSignalRTS, 0);
	// the UART (and DTR) only come on with the first request: a CR is sent so
	// that it is on for certain before the divider is read or touched (a
	// modem at the other speed sees one odd byte, which it ignores)
	Write(_L8("\r"));
	Drain(300);
	TBuf8<80> m;
	m.Format(_L8("Serial port open: UART %s, divider %d"), UartOn() ? "on" : "off", Divider());
	Log(m);
	return KErrNone;
	}

TInt TSpSerial::Write(const TDesC8& aData)
	{
	if (!iOpen)
		return KErrNotReady;
	TRequestStatus ws;
	iComm.Write(ws, aData);
	User::WaitForRequest(ws);
	return ws.Int();
	}

TInt TSpSerial::Read(TDes8& aBuf, TInt aMs)
	{
	aBuf.Zero();
	if (!iOpen || !iTimerOpen)
		return KErrNotReady;
	TRequestStatus ts, rs;
	iTimer.After(ts, aMs * 1000);
	iComm.ReadOneOrMore(rs, aBuf);
	User::WaitForRequest(rs, ts);
	TInt r;
	if (rs == KRequestPending)
		{
		iComm.ReadCancel();
		User::WaitForRequest(rs);
		r = aBuf.Length() ? KErrNone : KErrTimedOut;
		}
	else
		{
		r = rs.Int();
		iTimer.Cancel();
		User::WaitForRequest(ts);
		}
	return r;
	}

// a command and what came back: KErrNone OK, KErrGeneral ERROR,
// KErrTimedOut nothing; aReply gets all of it. Logged while gTalkLog.
TInt TSpSerial::Talk(const TDesC8& aCommand, TInt aTimeoutMs, TDes8* aReply)
	{
	if (!iOpen)
		return KErrNotReady;
	iComm.ResetBuffers();
	TInt r = Write(aCommand);
	if (r != KErrNone)
		return r;
	TBuf8<300> reply;
	TBuf8<128> piece;
	TUint t0 = User::TickCount();
	r = KErrTimedOut;
	while (Ms(t0) < (TUint)aTimeoutMs)
		{
		if (Read(piece, 100) != KErrNone)
			continue;
		if (reply.Length() + piece.Length() <= reply.MaxLength())
			reply.Append(piece);
		else
			reply = piece;
		TInt cp = reply.Find(_L8("CONNECT"));
		if (cp >= 0)
			{
			// the rest of the CONNECT line ("CONNECT 115200\r\n") is not data:
			// read it to its end
			TUint c0 = User::TickCount();
			while (reply.Mid(cp).Locate('\n') < 0 && Ms(c0) < 300)
				{
				if (Read(piece, 50) == KErrNone && reply.Length() + piece.Length() <= reply.MaxLength())
					reply.Append(piece);
				}
			r = KErrNone;
			break;
			}
		if (reply.Find(_L8("OK\r\n")) >= 0)
			{
			r = KErrNone;
			break;
			}
		if (reply.Find(_L8("ERROR")) >= 0 || reply.Find(_L8("NO CARRIER")) >= 0 || reply.Find(_L8("BUSY")) >= 0)
			{
			r = KErrGeneral;
			break;
			}
		}
	if (aReply)
		aReply->Copy(reply.Left(Min(reply.Length(), aReply->MaxLength())));
	if (gTalkLog)
		{
		TBuf8<40> c;
		Printable(aCommand, c);
		TBuf8<100> p;
		Printable(reply, p);
		TBuf8<180> m;
		m.Format(_L8("> %S  < %S  (%d, %u ms)"), &c, &p, r, Ms(t0));
		Log(m);
		}
	return r;
	}

// reads and throws away for aMs (the guard times round +++: quiet from the
// Psion only, so whatever comes is taken)
void TSpSerial::Drain(TInt aMs)
	{
	TBuf8<256> junk;
	TUint t0 = User::TickCount();
	while (Ms(t0) < (TUint)aMs)
		Read(junk, 50);
	}

// a call hung up: +++ with quiet round it (still reading), then ATH
void TSpSerial::HangUp()
	{
	Drain(1100);
	Write(_L8("+++"));
	Drain(1100);
	Talk(_L8("ATH\r"), 2000);
	}

// The port closed. Any divider the helper wrote is forgotten now, while
// the port is closed (ERestore puts back the register only if it still
// holds what we wrote), so that the driver never writes it again later -
// when the helper ends, say - over another program's port.
void TSpSerial::Close()
	{
	if (iOpen)
		{
		iComm.Close();
		iOpen = EFalse;
		gSelf--;
		if (gKernOpen)
			gKern.Restore();
		}
	if (iTimerOpen)
		{
		iTimer.Close();
		iTimerOpen = EFalse;
		}
	if (iServerOpen)
		{
		iServer.Close();
		iServerOpen = EFalse;
		}
	}

// ----- finding and telling the modem ----------------------------------------------
static const TText8* const KUp[] = { 0, (const TText8*)"AT$SB=230400\r", (const TText8*)"ATB230400\r", (const TText8*)"AT+IPR=230400\r" };
static const TText8* const KDown[] = { 0, (const TText8*)"AT$SB=115200\r", (const TText8*)"ATB115200\r", (const TText8*)"AT+IPR=115200\r" };
static const TText8* const KHow[] = { (const TText8*)"", (const TText8*)"AT$SB", (const TText8*)"ATB", (const TText8*)"AT+IPR" };

// (aSet: the divider is set first; else the port is talked to as it is)
static TBool AnswersAt(TSpSerial& aPort, TInt aDiv, TBool aSet)
	{
	if (aSet && SetDivider(aDiv) != KErrNone)
		return EFalse;
	aPort.Drain(100);
	return aPort.Talk(_L8("AT\r"), 1000) == KErrNone;
	}

// the speed the port opened at: 230400 with the hook in (the divider says
// so on a 5mx; the emulator does not model it), else 115200
static TBool OpenedFast()
	{
	TInt d = Divider();
	if (d == KDiv230400 || d == KDiv115200)
		return d == KDiv230400;
	return HookOn();
	}

// Which speed the modem answers AT at: 115200, 230400, or 0 (nothing
// answers). First a plain AT at the speed the port opened at (no driver
// write), then at the other; then (aHangUp) +++ with quiet round it and ATH
// at each, for a modem still in a call (a Psion Internet call put down by
// the Psion does not always end the modem's). The port is left at the speed
// found (as it opened if none).
static TInt FindModem(TSpSerial& aPort, TBool aHangUp = ETrue)
	{
	TBool fast = OpenedFast();
	const TInt divs[2] = { fast ? KDiv230400 : KDiv115200, fast ? KDiv115200 : KDiv230400 };
	const TInt bauds[2] = { fast ? 230400 : 115200, fast ? 115200 : 230400 };
	for (TInt pass = 0; pass < (aHangUp ? 2 : 1); pass++)
		for (TInt i = 0; i < 2; i++)
			{
			if (pass)
				{
				TBuf8<100> m;
				TNum8 at(bauds[i], " bps");
				m.Format(_L8("No answer to AT: +++ and ATH at %S, in case the modem is still in a call"), &at);
				Log(m);
				if (SetDivider(divs[i]) != KErrNone)
					continue;
				aPort.HangUp();
				}
			if (AnswersAt(aPort, divs[i], pass || i))
				{
				gModemSpeed = bauds[i];
				TBuf8<80> m;
				TNum8 at(bauds[i], " bps"), opened(bauds[0], " bps");
				m.Format(_L8("The modem answers AT at %S (the port opened at %S)"), &at, &opened);
				Log(m);
				gS->iHelperSeq++;
				return bauds[i];
				}
			}
	gModemSpeed = 0;
	SetDivider(divs[0], ETrue);
	if (aHangUp)
		Step(_L8("No modem answered AT at 115,200 or 230,400 bps, even after +++ ATH"));
	return 0;
	}

// The modem and the Psion's port back at 115200 by the UART driver, from
// any state, with the hook out: ETrue if the modem answers AT
// at 115200 at the end.
static TBool ModemDown(TSpSerial& aPort)
	{
	TInt speed = FindModem(aPort);
	TBool ok = EFalse;
	if (speed == 230400)
		{
		// told back the way it was told; if that is not known, the first it takes
		TInt first = gS->iModemAt >= 1 && gS->iModemAt <= 3 ? gS->iModemAt : 1;
		TInt took = 0;
		for (TInt i = 0; i < 3 && !took; i++)
			{
			TInt h = (first - 1 + i) % 3 + 1;
			if (aPort.Talk(TPtrC8(KDown[h]), 1500) == KErrNone)
				took = h;
			}
		User::After(400000);
		SetDivider(KDiv115200);
		if (took)
			ok = aPort.Talk(_L8("AT\r"), 1000) == KErrNone || aPort.Talk(_L8("AT\r"), 1000) == KErrNone;
		TBuf8<160> m;
		if (ok)
			m.Format(_L8("The modem is back at 115,200 bps (%s=115200), confirmed with AT; the Psion too"), KHow[took]);
		else if (took)
			m.Format(_L8("The modem took %s=115200 but does not answer AT at 115,200 bps: switch it off and on"), KHow[took]);
		else
			m = _L8("The modem at 230,400 bps takes none of AT$SB=115200, ATB115200, AT+IPR=115200: switch it off and on");
		Step(m);
		}
	else if (speed == 115200)
		{
		SetDivider(KDiv115200, ETrue);       // (it answered at 115200: the port is there)
		ok = ETrue;
		Step(_L8("The modem and the Psion are at 115,200 bps"));
		}
	else
		Log(_L8("The Psion's port is at 115,200 bps; the modem did not answer"));
	gModemSpeed = ok ? 115200 : speed == 0 ? 0 : 230400;
	gS->iHelperSeq++;
	return ok;
	}

// ----- watching the port -----------------------------------------------------------
// what the page shows of the UART; called by the loop and while the helper waits
static void Tick()
	{
	gS->iBeat++;
	TBool on = UartOn();
	TInt div = on ? Divider() : -1;
	if (on != (gS->iUartOn != 0) || div != gS->iDivider)
		{
		gS->iUartOn = on;
		gS->iDivider = div;
		gS->iHelperSeq++;
		}
	}

// NIFMAN's link: up (or on its way up), by either stage number the 5mx uses
static TBool NifUp(RNif& aNif, TInt& aStage)
	{
	TBool active = EFalse;
	aNif.NetworkActive(active);
	TNifProgress p;
	aStage = aNif.Progress(p) == KErrNone ? p.iStage : -1;
	return active || (aStage >= EStartingSelection && aStage <= EConnectionOpen) || aStage == KStageLinkUp;
	}

// Opens the port. When it is in use (Psion Internet keeps COMM::0 for a
// while after a link has gone down, whether NetworkActive says so or not),
// waits up to aWaitSecs for it, asking NIFMAN to stop a link that is up
// after aStopMs. KErrNone, or why not (said, when a wait was asked for).
static TInt PortOpen(TSpSerial& aPort, TInt aWaitSecs, TInt aStopMs = 8000)
	{
	TInt r = aPort.Open();
	TUint t0 = User::TickCount();
	TBool said = EFalse, stopped = EFalse;
	while ((r == KErrAccessDenied || r == KErrInUse) && Ms(t0) < (TUint)aWaitSecs * 1000 && !gS->iQuit)
		{
		if (!said)
			{
			said = ETrue;
			TBuf8<120> m;
			m.Format(_L8("The serial port is in use (%d): waiting up to %d s for it"), r, aWaitSecs);
			Log(m);
			}
		if (!stopped && Ms(t0) >= (TUint)aStopMs)
			{
			stopped = ETrue;
			RNif nif;
			if (nif.Open() == KErrNone)
				{
				// (Stop is synchronous with no bound: only for a link that is up
				// or on its way, as psiglue does)
				TInt stage = -1;
				TBool up = NifUp(nif, stage);
				TBuf8<80> m;
				if (up)
					{
					m.Format(_L8("Psion Internet holds the port (stage %d): asking it to stop"), stage);
					Log(m);
					nif.Stop();
					}
				else
					{
					m.Format(_L8("Psion Internet is not up (stage %d) but something holds the port: waiting"), stage);
					Log(m);
					}
				nif.Close();
				}
			}
		Tick();
		User::After(aStopMs < 8000 ? 250000 : 1000000);
		r = aPort.Open();
		}
	if (r != KErrNone && aWaitSecs > 0)
		{
		TBuf8<140> m;
		m.Format(_L8("The serial port is in use (%d)%s: close the program using it"), r, said ? " after waiting" : "");
		Step(m);
		}
	else if (said && r == KErrNone)
		{
		TBuf8<60> m;
		m.Format(_L8("The serial port came free after %u s"), Ms(t0) / 1000);
		Log(m);
		}
	return r;
	}

// ----- 230400 always ----------------------------------------------------------------
// The modem and the Psion's port to 230400 (aFast) or 115200, the hook in or
// out to match; aSave: the modem's AT&W at the new speed too. aPort is open
// (as the hook was); it is closed and opened again across the change, and
// left open at the new speed when that worked. TSpSetup.
static TInt MoveTo(TSpSerial& aPort, TBool aFast, TBool aSave)
	{
	const TInt want = aFast ? 230400 : 115200;
	TInt speed = FindModem(aPort);
	if (!speed)
		return ESpSetupNoModem;
	// (an older PsiBoost may have told a modem that has AT$CS to switch PPP
	// calls itself: AT$CS=0 takes that back. Other modems answer ERROR,
	// which is fine.)
	aPort.Talk(_L8("AT$CS=0\r"), 1500);
	TBuf8<300> reply;
	TBuf8<180> m;
	if (speed != want)
		{
		TInt how = 0;
		TBuf8<96> said;
		TInt first = !aFast && gS->iModemAt >= 1 && gS->iModemAt <= 3 ? gS->iModemAt : 1;
		for (TInt i = 0; i < 3 && !how; i++)
			{
			TInt h = (first - 1 + i) % 3 + 1;
			if (aPort.Talk(TPtrC8(aFast ? KUp[h] : KDown[h]), 1500, &reply) == KErrNone)
				how = h;
			else
				{
				TBuf8<36> p;
				Printable(reply.Right(Min(reply.Length(), 30)), p);
				if (said.Length() + p.Length() + 3 <= said.MaxLength())
					{
					said.Append(p);
					said.Append(_L8(" | "));
					}
				}
			}
		if (!how)
			{
			TNum8 rate(want, " bps");
		m.Format(_L8("The modem takes none of AT$SB, ATB, AT+IPR for %S (it said: %S)"), &rate, &said);
			Step(m);
			return ESpSetupNoSpeed;
			}
		if (aFast)
			gS->iModemAt = how;
		m.Format(_L8("The modem took %s=%d: it changes after its OK"), KHow[how], want);
		Log(m);
		User::After(300000);
		}
	aPort.Close();
	TInt h = HookSet(aFast);
	if (h != KErrNone)
		return h == KErrInUse ? ESpSetupForeign : ESpSetupNoHook;
	if (PortOpen(aPort, 5) != KErrNone)
		return ESpSetupInUse;
	// on a 5mx the divider says whether the hook took (the emulator reads -1)
	TInt d = Divider();
	if (d >= 0 && d != (aFast ? KDiv230400 : KDiv115200))
		{
		m.Format(_L8("The port opened with divider %d, not %d: the hook did not take"), d, aFast ? KDiv230400 : KDiv115200);
		Step(m);
		return ESpSetupNotHooked;
		}
	reply.Zero();
	if (aPort.Talk(_L8("AT\r"), 1000, &reply) != KErrNone && aPort.Talk(_L8("AT\r"), 1500, &reply) != KErrNone)
		{
		TBuf8<40> p;
		Printable(reply, p);
		TNum8 rate(want, " bps");
		m.Format(_L8("No answer from the modem at %S after the change (it said: %S)"), &rate, &p);
		Step(m);
		return ESpSetupNoAnswer;
		}
	gModemSpeed = want;
	if (aSave)
		{
		reply.Zero();
		if (aPort.Talk(_L8("AT&W\r"), 4000, &reply) != KErrNone)
			{
			TBuf8<40> p;
			Printable(reply, p);
			TNum8 rate(want, " bps");
			m.Format(_L8("The modem would not save %S (AT&W said: %S)"), &rate, &p);
			Step(m);
			return ESpSetupNoSave;
			}
		}
	TNum8 rate(want, " bps");
	m.Format(_L8("The modem and the Psion are at %S%s, the hook %s"), &rate, aSave ? " (saved)" : "", aFast ? "in" : "out");
	Step(m);
	return ESpSetupOk;
	}

// after a failure: the hook out, and the modem back at 115200 and saved
static void PutBack()
	{
	HookSet(EFalse);
	TSpSerial port;
	if (PortOpen(port, 5) != KErrNone)
		return;
	if (ModemDown(port) && port.Talk(_L8("AT&W\r"), 4000) == KErrNone)
		Log(_L8("The modem saved 115,200 bps (AT&W)"));
	port.Close();
	}

// 230400 always with no modem (a cable to a computer, or a device whose
// speed is set by hand): only the hook in or out, no AT. A program that has
// the port open now keeps its speed until it opens it again. On a 5mx the
// divider says whether the hook took, if the port is free to look.
static TInt AlwaysNoModem(TBool aOn)
	{
	TInt h = HookSet(aOn);
	if (h != KErrNone)
		return h == KErrInUse ? ESpSetupForeign : ESpSetupNoHook;
	if (aOn)
		{
		TSpSerial port;
		if (PortOpen(port, 0) == KErrNone)
			{
			TInt d = Divider();
			port.Close();
			if (d >= 0 && d != KDiv230400)
				{
				TBuf8<100> m;
				m.Format(_L8("The port opened with divider %d, not %d: the hook did not take"), d, KDiv230400);
				Step(m);
				HookSet(EFalse);
				return ESpSetupNotHooked;
				}
			}
		}
	Step(aOn ? _L8("No modem: the hook is in - set the other end to 230,400 bps")
	         : _L8("No modem: the hook is out - set the other end back to 115,200 bps"));
	return ESpSetupOk;
	}

// 230400 always on (aOn) or off: TSpSetup. On: everything or nothing (the
// hook out and the modem back at 115200 if any step fails). Off: off on the
// Psion (the hook out) whatever the modem says, unless the port is in use
// or another driver has hooked it.
static TInt Always(TBool aOn)
	{
	Say(aOn ? _L8("Turning 230,400 bps always on") : _L8("Turning 230,400 bps always off"));
	TUint32 f;
	if (HookFound(f) == EPbForeign)
		{
		Step(_L8("The serial port is hooked by another driver: restart the Psion"));
		return ESpSetupForeign;
		}
	if (gS->iNoModem)
		return AlwaysNoModem(aOn);
	TSpSerial port;
	TInt r = PortOpen(port, 0);
	if (r != KErrNone)
		{
		TBuf8<120> m;
		m.Format(_L8("The serial port is in use (%d): Speed > Free the serial port, then try again"), r);
		Step(m);
		return ESpSetupInUse;
		}
	r = MoveTo(port, aOn, ETrue);
	port.Close();
	if (r == ESpSetupOk)
		return r;
	if (aOn)
		PutBack();
	else if (r != ESpSetupForeign)
		HookSet(EFalse);
	return r;
	}

// Speed > Free the serial port: Psion Internet stopped at once if it is up,
// then the port waited for (up to 20 s). ETrue when it is free.
static TBool FreePort()
	{
	Say(_L8("Freeing the serial port"));
	RNif nif;
	if (nif.Open() == KErrNone)
		{
		TInt stage = -1;
		if (NifUp(nif, stage))
			{
			TBuf8<80> m;
			m.Format(_L8("Psion Internet is up (stage %d): stopping it"), stage);
			Log(m);
			nif.Stop();
			}
		nif.Close();
		}
	TSpSerial port;
	if (PortOpen(port, KPortWaitSecs, 0) != KErrNone)
		return EFalse;                       // (PortOpen said why)
	port.Close();
	Step(_L8("The serial port is free"));
	return ETrue;
	}

// a quick look at the modem's speed: AT at the speed the port opens at, then
// at the other (no +++), only if the port is free. TSpShared::iModemSpeed.
static TInt CheckModem()
	{
	if (gS->iNoModem)
		return KSpModemNotLooked;            // (no modem: nothing to ask)
	TSpSerial port;
	if (PortOpen(port, 0) != KErrNone)
		{
		Log(_L8("Quick look at the modem: the serial port is in use, not looked"));
		return KSpModemPortBusy;
		}
	TBool fast = OpenedFast();
	TInt speed = FindModem(port, EFalse);
	port.Close();
	TBuf8<100> m;
	TNum8 at((TUint)speed, " bps"), opens(fast ? 230400 : 115200, " bps");
	if (speed > 0)
		m.Format(_L8("Quick look at the modem: it answers at %S (the port opens at %S)"), &at, &opens);
	else
		m.Format(_L8("Quick look at the modem: no answer (the port opens at %S)"), &opens);
	Log(m);
	return speed;
	}

// ----- the link test ------------------------------------------------------------------
// Two halves, at 115200 and then at 230400, on the same route and server;
// each two legs of iTestSecs: the download (chargen from your own server, or a
// file of known size and checksum over plain HTTP from a public server),
// checked byte by byte or by its checksum, then echo (both ways at once,
// checked byte by byte, with the first byte's round trip). Each half first
// puts the modem and the port at its speed as 230400 always does (MoveTo,
// not saved), so the 230400 half goes through the hook, as every program
// does. The modem route dials each leg with ATDT host:port; Psion Internet
// connects sockets over its call. At the end, the modem and the hook as
// 230400 always says. The verdict on the pair: sp_verdict (sptest.c).
static sp_result gR;
static TUint gLastIn;                        // tick of the last byte in
static TBool gStalled;

static TBool Stopping() { return gS->iTestStop != 0 || gS->iQuit != 0; }

static TBool Public() { return gS->iTestServer == ESpServerPublic; }

static void Got(TInt aN)
	{
	if (aN > 0)
		{
		gLastIn = User::TickCount();
		gStalled = EFalse;
		}
	else if (!gStalled && Ms(gLastIn) >= 3000)
		{
		gR.stalls++;
		gStalled = ETrue;
		TestNow(_L8("Nothing for 3 s: a stall"));
		}
	// the UART must be at the speed asked for, all the way through
	TInt d = Divider();
	if (UartOn() && d >= 0 && d != (gR.baud > 115200 ? KDiv230400 : KDiv115200))
		{
		if (gR.divider_ok)
			{
			TBuf8<80> m;
			m.Format(_L8("The port's divider is %d during the test (not %d)"), d, gR.baud > 115200 ? KDiv230400 : KDiv115200);
			Step(m);
			}
		gR.divider_ok = 0;
		}
	}

static void Progress(const TDesC8& aWhat, TUint aT0)
	{
	TBuf8<96> m(aWhat);
	TNum8 rate(gR.baud, " bps");
	m.AppendFormat(_L8(" at %S, %u s of %d"), &rate, Ms(aT0) / 1000, gS->iTestSecs);
	gS->iTestNow.Copy(m);
	gS->iHelperSeq++;
	}

// the download's or the echo's host and port, for this server (aTry: the
// public file to try, 0 or 1)
static void LegAddress(TBool aEcho, TInt aTry, TDes8& aHost, TInt& aPort)
	{
	if (Public())
		{
		if (aEcho)
			aHost = KPublicEchoHost;
		else
			aHost = TPtrC8(KPublicFiles[aTry].iHost);
		aPort = aEcho ? KPublicEchoPort : 80;
		}
	else
		{
		aHost = gS->iTestHost;
		aPort = gS->iTestBase + (aEcho ? 7 : 19);
		}
	}

// how many servers a leg may try
static TInt LegTries(TBool aEcho) { return Public() && !aEcho ? 2 : 1; }

// the HTTP request for the public file (the whole file; the leg ends with
// the time, or with the file)
static void GetRequest(TInt aTry, TDes8& aReq)
	{
	TPtrC8 path(KPublicFiles[aTry].iPath), host(KPublicFiles[aTry].iHost);
	aReq.Format(_L8("GET %S HTTP/1.0\r\nHost: %S\r\nUser-Agent: PsiBoost (Psion Series 5mx)\r\nConnection: close\r\n\r\n"), &path, &host);
	}

// The download's checker: chargen or HTTP, as the server is
class TSpDown
	{
public:
	void Init(TInt aTry);
	// aP: n bytes in; returns the bytes that were data (counted), with
	// gR.down_bad added to. ETrue when the file is whole (HTTP: the leg ends)
	TBool Feed(const TUint8* aP, TInt aN);
	// HTTP: the server has answered, and not with the file (another is tried)
	TBool Refused() const { return Public() && iH.header_done && iH.status != 200 && iH.status != 206; }
	TInt Status() const { return iH.status; }
	void End();                              // the checksum (HTTP, whole)
private:
	sp_chargen iC;
	sp_http iH;
	TInt iTry;
	};

void TSpDown::Init(TInt aTry)
	{
	iTry = aTry;
	if (Public())
		sp_http_init(&iH);
	else
		sp_chargen_init(&iC);
	}

TBool TSpDown::Feed(const TUint8* aP, TInt aN)
	{
	if (!Public())
		{
		gR.down_bad += sp_chargen_check(&iC, aP, aN);
		gR.down_bytes += (unsigned long)aN;
		return EFalse;
		}
	TInt body = sp_http_feed(&iH, aP, aN);
	if (!Refused())
		gR.down_bytes += (unsigned long)body;
	return sp_http_whole(&iH);
	}

void TSpDown::End()
	{
	if (!Public() || !iH.header_done || Refused())
		return;
	gR.down_bad += iH.bad;
	if (sp_http_whole(&iH))
		{
		unsigned char out[16];
		sp_md5_final(&iH.md5, out);
		if (iH.length == KPublicFiles[iTry].iSize && !sp_md5_is(out, KPublicFiles[iTry].iMd5))
			{
			gR.down_bad++;
			Step(_L8("The file came whole but its MD5 checksum is wrong"));
			}
		else
			Log(_L8("The file came whole; its MD5 checksum is right"));
		}
	else
		{
		TBuf8<120> m;
		TNum8 got((TUint)iH.body), all((TUint)iH.length, " bytes");
		m.Format(_L8("Download: %S of %S of the file in the time (size checked, not the checksum)"), &got, &all);
		Log(m);
		}
	}

// the echo's stream: text for the public echo, every byte value for your own server
static void EchoInit(sp_echo& aE)
	{
	if (Public())
		sp_echo_init_text(&aE, 0x5053 + Ms(0));
	else
		sp_echo_init(&aE, 0x5053 + Ms(0));
	}

// the modem route: one call to the leg's service, aEcho: echo, else the
// download (the second public file if the first cannot be had)
static void ModemLeg(TSpSerial& aPort, TBool aEcho)
	{
	TBuf8<64> host;
	TInt port;
	TInt tries = LegTries(aEcho), attempt = 0;
	TSpDown down;
	TUint t0 = 0;                            // the leg's clock: from the request (HTTP), else from the connection
	for (;;)
		{
		LegAddress(aEcho, attempt, host, port);
		TBuf8<120> dial(_L8("ATDT"));
		dial.Append(host);
		dial.AppendFormat(_L8(":%d\r"), port);
		TBuf8<300> reply;
		TBool connected = aPort.Talk(dial, 30000, &reply) == KErrNone && reply.Find(_L8("CONNECT")) >= 0;
		if (connected && !aEcho && Public())
			{
			// the request, and the server's first word (a refusal means the other server)
			TBuf8<200> req;
			GetRequest(attempt, req);
			t0 = User::TickCount();
			aPort.Write(req);
			down.Init(attempt);
			TBuf8<300> head;
			TUint h0 = User::TickCount();
			while (Ms(h0) < 20000 && !Stopping() && !down.Refused() && (!head.Length() || head.Find(_L8("\r\n\r\n")) < 0))
				{
				TBuf8<128> piece;
				if (aPort.Read(piece, 200) == KErrNone)
					{
					TInt nc = piece.Find(_L8("NO CARRIER"));
					if (nc >= 0)
						{
						connected = EFalse;
						break;
						}
					down.Feed(piece.Ptr(), piece.Length());
					if (head.Length() + piece.Length() <= head.MaxLength())
						head.Append(piece);
					else
						head = piece;
					}
				}
			if (down.Refused() || !connected)
				{
				TBuf8<120> m;
				m.Format(_L8("%S answered %d, not the file"), &host, down.Status());
				Log(m);
				if (connected)
					aPort.HangUp();
				connected = EFalse;
				}
			}
		if (connected)
			break;
		if (++attempt < tries)
			{
			Log(_L8("Trying the other public server"));
			continue;
			}
		gR.drops++;
		TBuf8<40> p;
		Printable(reply.Right(Min(reply.Length(), 30)), p);
		TBuf8<160> m;
		TNum8 rate(gR.baud, " bps");
		m.Format(_L8("No call to %S:%d at %S (the modem said: %S)"), &host, port, &rate, &p);
		Step(m);
		return;
		}
	if (!(!aEcho && Public()))
		down.Init(attempt);
	if (!t0)
		t0 = User::TickCount();
	gLastIn = t0;
	gStalled = EFalse;
	TBuf8<1024> buf;
	sp_echo e;
	EchoInit(e);
	TUint sent0 = 0;                         // the first echo chunk's tick (the round trip)
	TBool whole = EFalse;
	while (Ms(t0) < (TUint)gS->iTestSecs * 1000 && !Stopping() && !whole)
		{
		if (aEcho && e.sent - e.back < 2048)
			{
			TBuf8<128> out;
			out.SetLength(128);
			sp_echo_fill(&e, (unsigned char*)out.Ptr(), 128);
			if (!sent0)
				sent0 = User::TickCount();
			if (aPort.Write(out) != KErrNone)
				{
				gR.drops++;
				break;
				}
			}
		TInt r = aPort.Read(buf, aEcho ? 30 : 100);
		if (r != KErrNone && r != KErrTimedOut)
			{
			gR.drops++;
			break;
			}
		TInt n = buf.Length();
		if (n)
			{
			// the modem's NO CARRIER (the far end closed) is not data
			TInt nc = buf.Find(_L8("NO CARRIER"));
			TInt data = nc >= 0 ? nc : n;
			if (data)
				{
				if (aEcho)
					{
					if (!gR.rtt_ms && sent0)
						gR.rtt_ms = Ms(sent0);
					gR.echo_bad += sp_echo_check(&e, buf.Ptr(), data);
					}
				else
					whole = down.Feed(buf.Ptr(), data);
				}
			if (nc >= 0)
				{
				if (!whole)
					gR.drops++;
				break;
				}
			}
		Got(n);
		if ((Ms(t0) & 1023) < 40)
			Progress(aEcho ? _L8("Echo:") : _L8("Download:"), t0);
		}
	if (aEcho)
		{
		gR.echo_bytes = e.back;
		gR.echo_ms = Ms(t0);
		}
	else
		{
		gR.down_ms = Ms(t0);
		down.End();
		}
	aPort.HangUp();
	}

static void ModemTest()
	{
	TSpSerial port;
	if (PortOpen(port, KPortWaitSecs) != KErrNone)
		{
		gR.drops++;
		return;
		}
	TBool fast = gR.baud > 115200;
	if (MoveTo(port, fast, EFalse) != ESpSetupOk)
		{
		if (fast)
			gR.divider_ok = 0;               // (the line never got to 230400)
		else
			gR.drops++;
		port.Close();
		return;
		}
	// the command-mode check: AT and a long echoed line, 20 times
	TestNow(_L8("Checking the modem in command mode"));
	port.Talk(_L8("ATE1\r"), 1000);
	gTalkLog = EFalse;                       // (40 lines of the same: the count is logged instead)
	for (TInt i = 0; i < 20 && !Stopping(); i++)
		{
		TBuf8<300> reply;
		gR.cmds += 2;
		if (port.Talk(_L8("AT\r"), 1000, &reply) != KErrNone)
			gR.cmds_bad++;
		TBuf8<64> line(_L8(SP_LONG_LINE));
		line.Append('\r');
		reply.Zero();
		port.Talk(line, 1500, &reply);
		if (!sp_reply_ok(SP_LONG_LINE, (const char*)reply.Ptr(), reply.Length()))
			gR.cmds_bad++;
		Got(1);
		}
	gTalkLog = ETrue;
	{
	TBuf8<80> m;
	TNum8 rate(gR.baud, " bps");
	m.Format(_L8("Command mode at %S: %d of %d garbled"), &rate, gR.cmds_bad, gR.cmds);
	Log(m);
	}
	if (gR.cmds_bad == 0)
		{
		port.Talk(_L8("ATNET0\r"), 1000);    // (telnet handling off, where a modem has it)
		TestNow(_L8("Download"));
		ModemLeg(port, EFalse);
		if (!Stopping())
			{
			TestNow(_L8("Echo, both ways"));
			ModemLeg(port, ETrue);
			}
		}
	port.Close();
	}

// a socket connect, with the port watched while it waits
static TInt WaitConnect(RSocket& aSocket, TRequestStatus& aStatus, TUint aMs)
	{
	TUint w0 = User::TickCount();
	while (aStatus == KRequestPending && Ms(w0) < aMs && !Stopping())
		{
		Tick();
		User::After(KTickUs);
		}
	if (aStatus == KRequestPending)
		{
		aSocket.CancelConnect();
		User::WaitForRequest(aStatus);
		return KErrTimedOut;
		}
	User::WaitForRequest(aStatus);
	return aStatus.Int();
	}

// Psion Internet: a name to an address over the call (which this starts)
static TInt Resolve(RSocketServ& aSs, const TDesC8& aHost, TInetAddr& aAddr)
	{
	TBuf<64> host;
	host.Copy(aHost);
	if (aAddr.Input(host) == KErrNone)
		return KErrNone;
	RHostResolver hr;
	TNameEntry ne;
	TInt r = hr.Open(aSs, KAfInet, KProtocolInetUdp);
	if (r == KErrNone)
		{
		TRequestStatus st;
		hr.GetByName(host, ne, st);
		TUint t0 = User::TickCount();
		while (st == KRequestPending && Ms(t0) < 90000 && !Stopping())
			{
			Tick();
			User::After(KTickUs);
			}
		if (st == KRequestPending)
			{
			hr.Cancel();
			User::WaitForRequest(st);
			r = KErrTimedOut;
			}
		else
			{
			User::WaitForRequest(st);
			r = st.Int();
			}
		hr.Close();
		}
	if (r != KErrNone)
		{
		TBuf8<100> m;
		m.Format(_L8("The name %S was not found (%d)"), &aHost, r);
		Step(m);
		return r;
		}
	aAddr = TInetAddr::Cast(ne().iAddr);
	return KErrNone;
	}

// Psion Internet: a socket to the leg's service over the call (the second
// public file if the first cannot be had)
static void SocketLeg(RSocketServ& aSs, TBool aEcho)
	{
	TBuf8<64> host;
	TInt port;
	TInt tries = LegTries(aEcho), attempt = 0;
	RSocket s;
	TSpDown down;
	TUint t0 = 0;
	RTimer timer;
	if (timer.CreateLocal() != KErrNone)
		{
		gR.drops++;
		return;
		}
	for (;;)
		{
		LegAddress(aEcho, attempt, host, port);
		TInetAddr a;
		TInt r = Resolve(aSs, host, a);
		if (r == KErrNone)
			r = s.Open(aSs, KAfInet, KSockStream, KProtocolInetTcp);
		if (r == KErrNone)
			{
			a.SetPort(port);
			TRequestStatus cs;
			s.Connect(a, cs);
			r = WaitConnect(s, cs, 90000);
			if (r != KErrNone)
				{
				TBuf8<120> m;
				m.Format(_L8("No connection to %S:%d through Psion Internet (%d)"), &host, port, r);
				Step(m);
				s.Close();
				}
			}
		if (r == KErrNone && !aEcho && Public())
			{
			TBuf8<200> req;
			GetRequest(attempt, req);
			TRequestStatus ws;
			t0 = User::TickCount();
			s.Write(req, ws);
			User::WaitForRequest(ws);
			down.Init(attempt);
			TBuf8<300> head;
			TUint h0 = User::TickCount();
			while (Ms(h0) < 20000 && !Stopping() && !down.Refused() && (!head.Length() || head.Find(_L8("\r\n\r\n")) < 0))
				{
				TBuf8<128> piece;
				TSockXfrLength len;
				TRequestStatus rs, ts;
				s.RecvOneOrMore(piece, 0, rs, len);
				timer.After(ts, 1000000);
				User::WaitForRequest(rs, ts);
				if (rs == KRequestPending)
					{
					s.CancelRecv();
					User::WaitForRequest(rs);
					Tick();
					continue;
					}
				timer.Cancel();
				User::WaitForRequest(ts);
				if (rs.Int() != KErrNone)
					{
					r = rs.Int();
					break;
					}
				down.Feed(piece.Ptr(), piece.Length());
				if (head.Length() + piece.Length() <= head.MaxLength())
					head.Append(piece);
				else
					head = piece;
				}
			if (down.Refused() || r != KErrNone)
				{
				TBuf8<120> m;
				m.Format(_L8("%S answered %d, not the file (%d)"), &host, down.Status(), r);
				Log(m);
				s.Close();
				r = KErrGeneral;
				}
			}
		if (r == KErrNone)
			break;
		if (++attempt < tries)
			{
			Log(_L8("Trying the other public server"));
			continue;
			}
		gR.drops++;
		timer.Close();
		return;
		}
	if (!(!aEcho && Public()))
		down.Init(attempt);
	if (!t0)
		t0 = User::TickCount();
	gLastIn = t0;
	gStalled = EFalse;
	TBuf8<1024> buf;
	sp_echo e;
	EchoInit(e);
	TUint sent0 = 0;
	TBool whole = EFalse;
	while (Ms(t0) < (TUint)gS->iTestSecs * 1000 && !Stopping() && !whole)
		{
		if (aEcho && e.sent - e.back < 4096)
			{
			TBuf8<512> out;
			out.SetLength(512);
			sp_echo_fill(&e, (unsigned char*)out.Ptr(), 512);
			if (!sent0)
				sent0 = User::TickCount();
			TRequestStatus ws;
			s.Write(out, ws);
			User::WaitForRequest(ws);
			if (ws.Int() != KErrNone)
				{
				gR.drops++;
				break;
				}
			}
		TSockXfrLength len;
		TRequestStatus rs, ts;
		buf.Zero();
		s.RecvOneOrMore(buf, 0, rs, len);
		timer.After(ts, 100000);
		User::WaitForRequest(rs, ts);
		TBool closed = EFalse;
		if (rs == KRequestPending)
			{
			s.CancelRecv();
			User::WaitForRequest(rs);
			}
		else
			{
			timer.Cancel();
			User::WaitForRequest(ts);
			if (rs.Int() != KErrNone && buf.Length() == 0)
				closed = ETrue;              // (the far end closed, the link dropped, or the server went)
			}
		TInt n = buf.Length();
		if (n)
			{
			if (aEcho)
				{
				if (!gR.rtt_ms && sent0)
					gR.rtt_ms = Ms(sent0);
				gR.echo_bad += sp_echo_check(&e, buf.Ptr(), n);
				}
			else
				whole = down.Feed(buf.Ptr(), n);
			}
		if (closed)
			{
			if (!whole)
				gR.drops++;
			break;
			}
		Got(n);
		Tick();
		if ((Ms(t0) & 1023) < 120)
			Progress(aEcho ? _L8("Echo:") : _L8("Download:"), t0);
		}
	timer.Close();
	if (aEcho)
		{
		gR.echo_bytes = e.back;
		gR.echo_ms = Ms(t0);
		}
	else
		{
		gR.down_ms = Ms(t0);
		down.End();
		}
	s.Close();
	}

// The test's Psion Internet half: the modem and the port at this half's
// speed (MoveTo: the 230400 half through the hook), then Psion Internet
// started as PsiTerm, PsiMail and PsiWeb start it. With a start command in
// Connection settings (ATDT777 on some Wi-Fi modems), the modem is told to start PPP
// on the serial port, and the Psion's own dial-up takes the line (psiglue's
// StartPpp), opening the port at 115200 - 230400 through the hook; with
// none, the Psion's own dial-up dials.
static TInt PppStart(TBool aFast)
	{
	TPsiLink link;
	link.SetDefaults();
	TBool loaded = gFs && link.Load(*gFs);
	TSpSerial port;
	TInt r = PortOpen(port, KPortWaitSecs);
	if (r != KErrNone)
		return r;
	if (MoveTo(port, aFast, EFalse) != ESpSetupOk)
		{
		port.Close();
		if (aFast)
			gR.divider_ok = 0;
		return KErrCouldNotConnect;
		}
	if (!loaded || !link.iPppStart.Length())
		{
		port.Close();
		return KErrNone;
		}
	TBuf8<48> cmd;
	cmd.Copy(link.iPppStart.Left(Min(link.iPppStart.Length(), 40)));
	TBuf8<96> m(_L8("Starting Psion Internet with "));
	m.Append(cmd);
	TNum8 rate(gModemSpeed, " bps");
	m.AppendFormat(_L8(" at %S"), &rate);
	TestNow(m);
	TBuf8<48> shown(cmd);
	cmd.Append('\r');
	TBuf8<300> reply;
	r = port.Talk(cmd, 25000, &reply);
	port.Close();
	TInt cp = reply.Find(_L8("CONNECT"));
	if (cp < 0)
		{
		TBuf8<40> p;
		Printable(reply.Right(Min(reply.Length(), 30)), p);
		TBuf8<160> f;
		f.Format(_L8("No CONNECT from the modem after %S at %S (it said: %S)"), &shown, &rate, &p);
		Step(f);
		return KErrCouldNotConnect;
		}
	TBuf8<60> p;
	Printable(reply.Mid(cp).Left(Min(reply.Length() - cp, 40)), p);
	TBuf8<100> c(_L8("The modem said: "));
	c.Append(p);
	Log(c);
	return KErrNone;
	}

// Psion Internet hung up, and the port waited for (so the next thing
// starts afresh)
static void PppStop()
	{
	RNif nif;
	if (nif.Open() == KErrNone)
		{
		TInt stage = -1;
		if (NifUp(nif, stage))
			nif.Stop();
		nif.Close();
		}
	for (TInt i = 0; i < 320 && UartOn(); i++)
		{
		Tick();
		User::After(KTickUs);
		}
	}

static void PppTest()
	{
	// a call already up: put down first, so the modem can be told while the
	// port is free, and the call starts at the speed tested
	{
	RNif nif;
	TBool active = EFalse;
	if (nif.Open() == KErrNone)
		{
		nif.NetworkActive(active);
		nif.Close();
		}
	if (active)
		{
		TestNow(_L8("Hanging up Psion Internet first, so the test starts afresh"));
		PppStop();
		}
	}
	if (PppStart(gR.baud > 115200) != KErrNone)
		{
		if (gR.divider_ok)
			gR.drops++;
		return;
		}
	RSocketServ ss;
	if (ss.Connect() != KErrNone)
		{
		gR.drops++;
		return;
		}
	TestNow(_L8("Connecting through Psion Internet"));
	SocketLeg(ss, EFalse);
	if (!Stopping() && !gR.drops)
		{
		TestNow(_L8("Echo, both ways"));
		SocketLeg(ss, ETrue);
		}
	// NIFMAN's own errors, then the call ended
	RNif nif;
	if (nif.Open() == KErrNone)
		{
		TNifProgress p;
		if (nif.LastProgressError(p) == KErrNone && p.iError != KErrNone)
			{
			gR.link_errors++;
			TBuf8<80> m;
			m.Format(_L8("Psion Internet reported error %d at stage %d"), p.iError, p.iStage);
			Log(m);
			}
		nif.Close();
		}
	ss.Close();
	PppStop();
	}

// ----- Tools > Send log to server --------------------------------------------------
// PsiBoost.log POSTed to PsiProxy's /_putlog (PsiProxy keeps it on the
// computer it runs on), as PsiTerm, PsiMail and PsiWeb send theirs: to the
// proxy in Connection settings (TPsiLink::DevBox), and only when one is set
// and in use. Nothing is built in. Always over the modem route
// (the helper's own port, ATDT host:port, at whichever speed the modem
// answers), whatever route Connection settings say: Psion Internet needs
// NIFMAN, whose link is not to be trusted when it says it is up, and the
// log must get through when everything else has failed. A Psion Internet
// call that holds the port is stopped first (PortOpen).
static TInt SendLog()
	{
	TPsiLink link;
	link.SetDefaults();
	TBuf8<64> host;
	TInt port = 0;
	if (!gFs || !link.Load(*gFs) || !link.ProxyUsable() || !link.DevBox(*gFs, host, port))
		{
		Say(_L8("Send log: not available - set a proxy in Connection settings"));
		return KErrNotFound;
		}
	// the log
	HBufC8* body = HBufC8::New(36 * 1024);
	if (!body)
		return KErrNoMemory;
	TPtr8 b = body->Des();
	b.Copy(_L8("==== PsiBoost.log ====\n"));
	{
	RFile f;
	if (f.Open(*gFs, gLogFile, EFileRead | EFileShareAny) == KErrNone)
		{
		TInt size = 0;
		f.Size(size);
		TInt room = b.MaxLength() - b.Length();
		if (size > room)
			{
			TInt pos = size - room;          // (the end, if it is long)
			f.Seek(ESeekStart, pos);
			}
		TPtr8 rest((TUint8*)b.Ptr() + b.Length(), 0, room);
		f.Read(rest);
		b.SetLength(b.Length() + rest.Length());
		f.Close();
		}
	}
	TBuf8<240> head;
	head.Format(_L8("POST /_putlog?app=PsiBoost&ver=%S HTTP/1.0\r\nHost: %S:%d\r\nUser-Agent: PsiBoost (Psion Series 5mx)\r\n"
		"Content-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"), &KSpVersion8, &host, port, b.Length());
	{
	TBuf8<120> m;
	TNum8 size(b.Length(), " bytes");
	m.Format(_L8("Send log over the modem route to %S:%d (%S)"), &host, port, &size);
	TestNow(m);
	}
	TBuf8<200> reply;
	TSpSerial sp;
	TInt r = PortOpen(sp, KPortWaitSecs);
	if (r == KErrNone)
		{
		if (!FindModem(sp))
			r = KErrCouldNotConnect;
		else
			{
			sp.Talk(_L8("ATNET0\r"), 1000);
			TBuf8<100> dial(_L8("ATDT"));
			dial.Append(host);
			dial.AppendFormat(_L8(":%d\r"), port);
			TBuf8<300> cr;
			if (sp.Talk(dial, 30000, &cr) != KErrNone || cr.Find(_L8("CONNECT")) < 0)
				{
				TBuf8<40> p;
				Printable(cr.Right(Min(cr.Length(), 30)), p);
				TBuf8<160> m;
				TNum8 rate(gModemSpeed, " bps");
				m.Format(_L8("No call to %S:%d at %S (the modem said: %S)"), &host, port, &rate, &p);
				Step(m);
				r = KErrCouldNotConnect;
				}
			else
				{
				sp.Write(head);
				r = sp.Write(b);
				TUint t0 = User::TickCount();
				TBuf8<128> piece;
				while (reply.Find(_L8("\r\n")) < 0 && Ms(t0) < 15000)
					if (sp.Read(piece, 200) == KErrNone && reply.Length() + piece.Length() <= reply.MaxLength())
						reply.Append(piece);
				sp.HangUp();
				}
			}
		sp.Close();
		}
	delete body;
	TBool ok = r == KErrNone && reply.Find(_L8(" 200")) >= 0;
	TBuf8<96> m;
	if (ok)
		{
		TNum8 rate(gModemSpeed, " bps");
		m.Format(_L8("Log sent to %S at %S"), &host, &rate);
		}
	else if (r == KErrNone)
		{
		TBuf8<30> p;
		Printable(reply.Left(Min(reply.Length(), 24)), p);
		m.Format(_L8("The log was not taken by the server (it said: %S)"), &p);
		}
	else
		m.Format(_L8("The log was not sent (%d): see the log's last lines"), r);
	Say(m);
	return ok ? KErrNone : KErrGeneral;
	}

// one half: the legs at gR.baud, and the numbers to the app
static void RunHalf(TSpHalf& aOut)
	{
	gR.ran = 1;
	if (gR.route)
		PppTest();
	else
		ModemTest();
	aOut.iDownRate = sp_rate(gR.down_bytes, gR.down_ms);
	aOut.iEchoRate = sp_rate(gR.echo_bytes, gR.echo_ms);
	aOut.iRtt = gR.rtt_ms;
	aOut.iStalls = gR.stalls;
	aOut.iBad = (TInt)(gR.down_bad + gR.echo_bad);
	aOut.iDrops = gR.drops;
	aOut.iDone = 1;
	gS->iHelperSeq++;
	TBuf8<200> d;
	TNum8 rate(gR.baud, " bps"), down(gR.down_bytes, " bytes"), downMs(gR.down_ms, " ms"), downBad(gR.down_bad),
		echo(gR.echo_bytes, " bytes"), echoMs(gR.echo_ms, " ms"), echoBad(gR.echo_bad), rtt(gR.rtt_ms, " ms");
	d.Format(_L8("At %S: down %S in %S (%S bad), echo %S in %S (%S bad), rtt %S, cmds %d/%d bad, stalls %d, drops %d"),
		&rate, &down, &downMs, &downBad, &echo, &echoMs, &echoBad, &rtt, gR.cmds_bad, gR.cmds, gR.stalls, gR.drops);
	Log(d);
	}

// after the test: the modem and the hook as 230400 always says (not saved:
// what the modem has saved is already that)
static void Settle()
	{
	TBool always = gS->iAuto != 0;
	if (gS->iNoModem)
		{
		HookSet(always);                     // (no modem: only the hook)
		return;
		}
	TSpSerial port;
	TInt r = ESpSetupInUse;
	if (PortOpen(port, 10) == KErrNone)
		{
		r = MoveTo(port, always, EFalse);
		port.Close();
		}
	if (r == ESpSetupOk)
		{
		Log(always ? _L8("After the test: the modem at 230,400 bps and the hook in, as 230,400 bps always says")
		           : _L8("After the test: the modem at 115,200 bps and the hook out, as 230,400 bps always says"));
		return;
		}
	if (!always)
		HookSet(EFalse);
	Say(always ? _L8("After the test the modem is not at 230,400 bps: turn 230,400 bps always off and on again")
	           : _L8("After the test the modem did not answer at 115,200 bps: see the log"));
	}

static void RunTest()
	{
	sp_result r115, r230;
	Mem::FillZ(&r115, sizeof(r115));
	Mem::FillZ(&r230, sizeof(r230));
	gS->iTestStop = 0;
	gS->iTestState = ESpTestRunning;
	gS->iTestPass = 0;
	gS->iTestVerdict.Zero();
	Mem::FillZ(&gS->iTestHalf[0], sizeof(TSpHalf));
	Mem::FillZ(&gS->iTestHalf[1], sizeof(TSpHalf));
	gLastStep.Zero();
	TBuf8<200> m;
	m.Format(_L8("Link test on %s, %d s each leg, %s, 230,400 bps always %s"),
		gS->iTestRoute ? "Psion Internet" : "the modem route", gS->iTestSecs,
		Public() ? "public servers" : "your own server", gS->iAuto ? "on" : "off");
	if (!Public())
		m.AppendFormat(_L8(" (%S:%d)"), &gS->iTestHost, gS->iTestBase);
	TestNow(m);
	for (TInt half = 0; half < 2 && !Stopping(); half++)
		{
		Mem::FillZ(&gR, sizeof(gR));
		gR.baud = half ? 230400 : 115200;
		gR.route = gS->iTestRoute;
		gR.divider_ok = 1;
		TestNow(half ? _L8("Now at 230,400 bps: 115,200 bps through the hook") : _L8("First at 115,200 bps, to compare"));
		RunHalf(gS->iTestHalf[half]);
		if (half)
			r230 = gR;
		else
			{
			r115 = gR;
			char why[100];
			sp_why(&r115, why, sizeof(why));
			if (why[0])
				break;                       // (no point in the second half)
			}
		}
	char text[200];
	TBool stopped = Stopping();
	TInt pass = stopped ? 0 : sp_verdict(&r115, &r230, text, sizeof(text));
	if (stopped)
		gS->iTestVerdict = _L8("Stopped before the end");
	else
		{
		gS->iTestVerdict.Copy(TPtrC8((const TUint8*)text));
		// failed before any data: the step that failed says why
		if (!pass && !r230.down_bytes && !r230.echo_bytes && gLastStep.Length())
			{
			TInt room = gS->iTestVerdict.MaxLength() - gS->iTestVerdict.Length() - 3;
			if (room > 20)
				{
				gS->iTestVerdict.Append(_L8(" ["));
				gS->iTestVerdict.Append(gLastStep.Left(Min(gLastStep.Length(), room)));
				gS->iTestVerdict.Append(_L8("]"));
				}
			}
		}
	gS->iTestPass = pass;
	Log(gS->iTestVerdict);
	TBool quit = gS->iQuit != 0;
	gS->iQuit = 0;                           // (Settle must run even when asked to end)
	Settle();
	if (quit)
		gS->iQuit = 1;
	gS->iTestState = ESpTestDone;
	gS->iHelperSeq++;
	}

// ----- the loop -------------------------------------------------------------------
static void Run()
	{
	TUint appBeat = gS->iAppBeat, appBeatAt = User::TickCount();
	// (a test asked for as the helper was started: run it first)
	TUint testSeen = gS->iTestReq - (gS->iTestState == ESpTestRunning ? 1 : 0);
	for (;;)
		{
		Tick();
		TBool on = gS->iUartOn != 0;
		if (gS->iAppBeat != appBeat)
			{
			appBeat = gS->iAppBeat;
			appBeatAt = User::TickCount();
			}
		if (gS->iSetupReq != gS->iSetupDone)
			{
			TUint want = gS->iSetupReq;
			TBool wantOn = gS->iSetupWant != 0;
			TInt r = Always(wantOn);
			gS->iSetupResult = r;
			gS->iSetupNowOn = wantOn ? r == ESpSetupOk : r == ESpSetupInUse;
			gS->iModemSpeed = r == ESpSetupOk && !gS->iNoModem ? (wantOn ? 230400 : 115200) : KSpModemNotLooked;
			gS->iSetupDone = want;
			gS->iHelperSeq++;
			continue;
			}
		if (gS->iFreeReq != gS->iFreeDone)
			{
			TUint want = gS->iFreeReq;
			gS->iFreeResult = FreePort();
			gS->iFreeDone = want;
			gS->iHelperSeq++;
			continue;
			}
		if (gS->iCheckReq != gS->iCheckDone)
			{
			TUint want = gS->iCheckReq;
			gS->iModemSpeed = CheckModem();
			gS->iCheckDone = want;
			gS->iHelperSeq++;
			continue;
			}
		if (gS->iTestReq != testSeen)
			{
			testSeen = gS->iTestReq;
			RunTest();
			continue;
			}
		if (gS->iLogReq != gS->iLogDone)
			{
			TUint want = gS->iLogReq;
			gS->iLogOk = SendLog() == KErrNone;
			gS->iLogDone = want;
			gS->iHelperSeq++;
			continue;
			}
		// the end: asked to, or PsiBoost has closed (or died) and nothing is
		// left to do (a test keeps it running until the app has seen the result)
		TBool appOpen = gS->iAppOpen && Ms(appBeatAt) < (TUint)KAppGoneMs;
		TBool idle = !appOpen && gS->iTestState != ESpTestRunning;
		if (gS->iQuit || idle)
			break;
		User::After(on ? KTickUs : KIdleTickUs);
		}
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RChunk chunk;
	if (chunk.OpenGlobal(KSpChunk, EFalse) != KErrNone)
		{
		delete cleanup;
		return KErrNotFound;                 // (started by PsiBoost.app only)
		}
	gS = (TSpShared*)chunk.Base();
	if (gS->iMagic != KSpMagic || gS->iLayout != KSpLayout)
		{
		chunk.Close();
		delete cleanup;
		return KErrNotSupported;
		}
	gS->iState = ESpStarting;
	gS->iVersion = KSpVersion8;
	gS->iHelperSeq++;
	RFs fs;
	if (fs.Connect() == KErrNone)
		gFs = &fs;
	{
	TParsePtrC me(RProcess().FileName());
	gLogFile = me.DriveAndPath();
	gLogFile.Append(KLogName);
	}
	// the drivers: for the ROM they were made for only
	TInt r = KErrNotSupported;
	TMachineInfoV1Buf mi;
	if (UserHal::MachineInfo(mi) == KErrNone)
		{
		const TVersion& v = mi().iRomVersion;
		if (v.iMajor == KPbRomMajor && v.iMinor == KPbRomMinor && v.iBuild == KPbRomBuild)
			{
			r = User::LoadLogicalDevice(KPbUartFile);
			if (r == KErrNone || r == KErrAlreadyExists)
				r = gKern.Open();
			if (r == KErrNone && ((TUint)gKern.Version() >> 16) != (TUint)KPbUartMagic)
				{
				gKern.Close();
				r = KErrNotSupported;
				}
			gKernOpen = r == KErrNone;
			if (r == KErrNone)
				{
				r = User::LoadLogicalDevice(KPbHookFile);
				if (r == KErrNone || r == KErrAlreadyExists)
					r = gHook.Open();
				if (r == KErrNone && ((TUint)gHook.Version() >> 16) != (TUint)KPbHookMagic)
					{
					gHook.Close();
					r = KErrNotSupported;
					}
				gHookOpen = r == KErrNone;
				}
			}
		}
	gS->iDriver = r;
	if (r == KErrNone)
		{
		// above the program in front: the serial work keeps its timing
		RProcess().SetPriority(EPriorityHigh);
		gS->iState = ESpRunning;
		gS->iHelperSeq++;
		TBuf8<120> m;
		m.Format(_L8("PsiBoost's helper %S started: 230,400 bps always %s, hook %s, port %s"),
			&KSpVersion8, gS->iAuto ? "on" : "off", HookOn() ? "in" : "out", UartOn() ? "open" : "closed");
		Log(m);
		Run();
		Log(_L8("PsiBoost's helper ended"));
		}
	else
		{
		TBuf8<80> m;
		m.Format(_L8("PsiBoost's helper %S: a device driver did not load (%d)"), &KSpVersion8, r);
		Log(m);
		}
	if (gHookOpen)
		gHook.Close();
	if (gKernOpen)
		gKern.Close();
	gS->iState = ESpStopped;
	gS->iUartOn = 0;
	gS->iHelperSeq++;
	if (gFs)
		fs.Close();
	chunk.Close();
	delete cleanup;
	return r;
	}
