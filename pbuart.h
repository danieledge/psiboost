// pbuart.h - the user side of PBUART21.LDD, PsiBoost's UART driver, used by
// the helper (pboostd.exe) to read UART2's registers and to set its baud
// divider while the helper's own port is open. See docs/how-it-works.md.
//
// Shared by the driver (uart/pbuart.cpp) and the helper.
#ifndef PBUART_H
#define PBUART_H
#include <e32std.h>

// The device's name and the file's carry the version: a driver stays loaded
// until the Psion restarts, and EPOC knows a loaded driver by its file name,
// so a newer driver must have new names to be loaded beside the old one.
_LIT(KPbUartName, "PsiBoostUart21");
_LIT(KPbUartFile, "PBUART21.LDD");       // \System\Libs\ on any drive
const TInt KPbUartMajor = 0;
const TInt KPbUartMinor = 21;
const TInt KPbUartBuild = 1;
const TInt KPbUartMagic = 0x5055;        // "PU", the top half of EPuVersion's answer

// DoControl functions. No function takes a user pointer: every answer is the
// return value, so the driver never touches user memory.
enum TPbUartControl
	{
	EPuVersion = 0,      // -> KPbUartMagic<<16 | major<<8 | minor
	EPuCpuMode = 1,      // -> CPSR & 0xff as the driver runs (0x13 is SVC)
	EPuRead32 = 2,       // a1 = offset from the register base -> low 16 bits (whole value latched)
	EPuRead8 = 3,        // a1 = offset -> the byte (0..255)
	EPuLatchHigh = 4,    // -> top 16 bits of the last EPuRead32
	EPuRestore = 9,      // puts UBRCR back if it still holds what was written -> 0
	EPuUbrcrSet = 10     // a1 = 1 (230400) or 3 (115200): UART2's divider, only while
	                     // the UART is on -> 0 or an error. Put back by EPuRestore.
	};

// UART2's registers, as offsets from the Windermere's register base
// (virtual 0x58000000 in the 1.05(260) ROM). The driver reads these only.
enum TPbUartReg
	{
	EPuRegFcr = 0x704, EPuRegUbrcr = 0x708, EPuRegCon = 0x70c, EPuRegFlg = 0x710, EPuRegIntM = 0x718
	};

class RPbUart : public RBusLogicalChannel
	{
public:
	inline TInt Open()
		{ return DoCreate(KPbUartName, TVersion(KPbUartMajor, KPbUartMinor, KPbUartBuild), NULL, KNullUnit, NULL, NULL); }
	inline TInt Version() { return DoControl(EPuVersion); }
	// a 32-bit register: two calls, but one read of the register
	inline TInt Read32(TInt aOffset, TUint32& aValue)
		{
		TInt lo = DoControl(EPuRead32, (TAny*)aOffset);
		if (lo < 0) return lo;
		TInt hi = DoControl(EPuLatchHigh);
		if (hi < 0) return hi;
		aValue = ((TUint32)hi << 16) | (TUint32)lo;
		return KErrNone;
		}
	inline TInt Read8(TInt aOffset) { return DoControl(EPuRead8, (TAny*)aOffset); }
	inline TInt Restore() { return DoControl(EPuRestore); }
	inline TInt UbrcrSet(TInt aValue) { return DoControl(EPuUbrcrSet, (TAny*)aValue); }
	};

#endif
