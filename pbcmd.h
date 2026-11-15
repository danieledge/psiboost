// pbcmd.h - (0.27) what PsiBoost.ctl, PsiBoost in the Control panel, asks
// PsiBoost.app to do: a bit mask, as the app's command-line tail ("PBcm" and
// the mask in hex) when it starts, or a window server message (EEventUser:
// KPbCmdEvent, then the mask) when it is open already. The app does them in
// this order, once its welcome (the first time) is done and nothing else
// is running: the other end, 230,400 bps always, then a test or freeing the
// port.
#ifndef PBCMD_H
#define PBCMD_H
#include <e32std.h>

const TUid KUidPbApp = { 0x01000A94 };
const TUint32 KPbCmdEvent = 0x6d634250;   // 'PBcm'

enum TPbCmd
	{
	EPbCmdOn = 0x01,            // 230,400 bps always on
	EPbCmdOff = 0x02,           // ...off
	EPbCmdModem = 0x04,         // the other end: a modem
	EPbCmdComputer = 0x08,      // ...a computer or other device
	EPbCmdTest = 0x10,          // Test the speed (its dialog)
	EPbCmdFree = 0x20,          // Free the serial port
	EPbCmdShow = 0x40           // only PsiBoost's page
	};

inline void PbCmdTail(TDes8& aTail, TUint aCmd)
	{
	aTail.Copy(_L8("PBcm"));
	aTail.AppendNumFixedWidth(aCmd, EHex, 2);
	}

// the mask in a tail, or 0
inline TUint PbCmdFromTail(const TDesC8& aTail)
	{
	TInt at = aTail.Find(_L8("PBcm"));
	if (at < 0 || at + 6 > aTail.Length())
		return 0;
	TLex8 lex(aTail.Mid(at + 4, 2));
	TUint v = 0;
	return lex.Val(v, EHex) == KErrNone ? v : 0;
	}
#endif
