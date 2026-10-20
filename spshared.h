// spshared.h - memory shared between PsiBoost.app and pboostd.exe, its
// helper. See README.md and docs/how-it-works.md.
//
// "230400 always": the hook (PBHOOK20.LDD, pbhook.h) makes every program
// that asks the RS-232 port for 115200 get 230400 from the first byte, and
// the modem is told 230400 and saves it (AT$SB=230400, ATB230400 or
// AT+IPR=230400, then AT&W at 230400). A recogniser on the CF card
// (PsiBoost.rdl) puts the hook back at start-up. The helper does the serial
// work, one request at a time, while PsiBoost waits with a busy message:
// turning 230400 always on or off, freeing the serial port, a quick look at
// the modem's speed, the link test and Send log. It holds the UART driver
// (PBUART21.LDD, pbuart.h, for the UART's registers) and the hook's. It runs
// while PsiBoost is open, and ends soon after it closes: the hook needs
// nothing running.
#ifndef SPSHARED_H
#define SPSHARED_H

#include <e32std.h>

_LIT(KSpChunk, "PsiBoostShared");
_LIT(KSpHelperExe, "pboostd.exe");
const TUint KSpMagic = 0x50535044;           // "PSPD"
// + 1 whenever TSpShared changes (2: the link test, 3: Send log, 4: the
// helper's version, the test's two halves, the infoprints; 5: 230400 for
// Psion Internet calls; 6: 230400 always, the hook). The first eight fields
// (to iAppSeq) never move: a PsiBoost of any version can ask a helper of any
// other to quit.
const TInt KSpLayout = 6;

// what turning 230400 always on or off came to (iSetupResult)
enum TSpSetup
	{
	ESpSetupNone,
	ESpSetupOk,                              // done, and checked with AT at the new speed
	ESpSetupInUse,                           // the serial port is held by another program
	ESpSetupNoModem,                         // nothing answers AT at 115200 or 230400
	ESpSetupNoSpeed,                         // the modem takes none of AT$SB, ATB, AT+IPR
	ESpSetupNoHook,                          // the hook could not be put in or taken out
	ESpSetupForeign,                         // the port is hooked by another driver: restart
	ESpSetupNotHooked,                       // the port opened at 115200 with the hook in
	ESpSetupNoAnswer,                        // no answer at the new speed (both put back)
	ESpSetupNoSave                           // the modem would not save its speed (AT&W)
	};

// the link test (Tools > Test the speed): iTestState
enum TSpTestState
	{
	ESpTestIdle,
	ESpTestRunning,
	ESpTestDone
	};

// the helper's state (iState)
enum TSpState
	{
	ESpStarting,
	ESpRunning,
	ESpStopped                               // it has ended (or never ran: no driver)
	};

// the test's server (iTestServer)
enum TSpTestServer
	{
	ESpServerPublic,                         // public servers on the internet (pboostd.cpp: KPublic...)
	ESpServerOwn                             // your own server: echo at iTestBase+7, chargen at +19 (server/stdsvc.py)
	};

// the modem's speed, as the last quick look found it (iModemSpeed)
const TInt KSpModemNotLooked = -1;           // (0: no answer at either speed)
const TInt KSpModemPortBusy = -2;            // the port was in use: not looked

// one half of the test (at 115200, then at 230400), as the app shows it
struct TSpHalf
	{
	volatile TInt iDone;                     // 1 this half ran (the numbers are good)
	volatile TUint iDownRate;                // bytes a second, the download
	volatile TUint iEchoRate;                // ...and the echo (both ways at once)
	volatile TUint iRtt;                     // the first echo byte's round trip, ms (0: none)
	volatile TInt iStalls;                   // 3 s with nothing coming in
	volatile TInt iBad;                      // bytes that were wrong
	volatile TInt iDrops;                    // calls or connections lost or not made
	};

struct TSpShared
	{
	TUint iMagic;
	TInt iLayout;
	// ---- the app (these eight never move) ----
	volatile TInt iAuto;                     // 230400 always (as PsiBoost.ini says)
	volatile TInt iNoModem;                  // (0.23) the other end is not a modem: the hook only, no AT
	volatile TInt iSpare2;
	volatile TInt iSpare3;
	volatile TInt iQuit;                     // the helper should end now
	volatile TUint iAppSeq;                  // + 1 when any of the above changes
	volatile TInt iSpare4;                   // (0.10 to 0.19: Say when the speed changes)
	// ---- the helper ----
	volatile TInt iState;                    // TSpState
	volatile TInt iDriver;                   // KErrNone, or why a driver could not be used
	volatile TInt iUartOn;                   // the serial port is open (UART2 enabled)
	volatile TInt iDivider;                  // UART2's divider: 3 is 115200, 1 is 230400, -1 unknown
	volatile TInt iModemAt;                  // how the modem was told 230400: 0 not known, 1 AT$SB, 2 ATB, 3 AT+IPR
	volatile TUint iBeat;                    // + 1 each time round the helper's loop
	TBuf8<96> iMsg;                          // the last thing that happened
	volatile TUint iHelperSeq;               // + 1 when anything above (but iBeat) changes
	TBuf8<8> iVersion;                       // the helper's version (the app restarts an older one)
	// ---- the link test: asked for by the app ----
	volatile TUint iTestReq;                 // + 1 to start one with the settings below
	volatile TInt iTestRoute;                // 0 the modem route, 1 Psion Internet
	volatile TInt iTestServer;               // TSpTestServer
	volatile TInt iTestSecs;                 // how long each leg lasts
	TBuf8<64> iTestHost;                     // your own server (ESpServerOwn), the host only
	volatile TInt iTestBase;                 // ...and its ports: base+7 echo, base+19 chargen
	volatile TInt iTestStop;                 // the app asks it to stop now
	// ---- ...and its results, from the helper ----
	volatile TInt iTestState;                // TSpTestState
	volatile TInt iTestPass;                 // 1 PASS, 0 FAIL (when done)
	TBuf8<100> iTestNow;                     // what it is doing
	TBuf8<200> iTestVerdict;                 // the verdict line
	TSpHalf iTestHalf[2];                    // at 115200, at 230400
	// ---- Tools > Send log to server (the proxy in Connection settings) ----
	volatile TUint iLogReq;                  // the app: + 1 to send PsiBoost.log
	volatile TUint iLogDone;                 // the helper: = iLogReq when it has tried
	volatile TInt iLogOk;                    // ...and whether it went (the why is in iMsg)
	// ---- the app is open (the helper runs while it is) ----
	volatile TInt iAppOpen;                  // 1 while PsiBoost is open
	volatile TUint iAppBeat;                 // + 1 twice a second while it is (a PsiBoost that died stops)
	// ---- 230400 always on or off: asked for by the app ----
	volatile TUint iSetupReq;                // + 1 to turn it iSetupWant
	volatile TInt iSetupWant;                // 1 on, 0 off
	// ---- ...and done by the helper ----
	volatile TUint iSetupDone;               // = iSetupReq when it has tried
	volatile TInt iSetupResult;              // TSpSetup
	volatile TInt iSetupNowOn;               // 230400 always as it ended: 1 on, 0 off
	// ---- Speed > Free the serial port ----
	volatile TUint iFreeReq;                 // the app: + 1
	volatile TUint iFreeDone;                // the helper: = iFreeReq when done
	volatile TInt iFreeResult;               // 1 the port is free, 0 still in use
	// ---- a quick look at the modem's speed (AT at both speeds, the port free) ----
	volatile TUint iCheckReq;                // the app: + 1
	volatile TUint iCheckDone;               // the helper: = iCheckReq when done
	volatile TInt iModemSpeed;               // 115200, 230400, 0 no answer, KSpModemNotLooked, KSpModemPortBusy
	};

#endif
