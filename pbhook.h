// pbhook.h - the user side of PBHOOK20.LDD, PsiBoost's serial-speed hook
// (0.20). See docs/how-it-works.md.
//
// Installed, it makes every program that
// asks the RS-232 port (UART2) for 115200 get 230400 on the wire, from the
// first byte, with no polling: it points the "Comm.Wind2" PDD factory at a
// constant copy of its vtable whose create slot stamps each new DComm
// channel with a copied channel vtable whose configure slot calls the ROM's
// configure, then writes divider 1 if the ROM wrote 3. Install checks the
// factory and both ROM slots first and writes nothing if any differ. The
// hook lives in RAM: a restart removes it.
//
// Shared by PsiBoost.app, pboostd.exe and the recogniser PsiBoost.rdl (all
// of which use PbFindFactory below), and the driver itself. Header only, no
// static data: the recogniser and the driver are DLLs.
#ifndef PBHOOK_H
#define PBHOOK_H
#include <e32std.h>

// (the device's name and the file's carry the version: a loaded driver stays
// until the Psion restarts, and EPOC knows it by its file name)
_LIT(KPbHookName, "PsiBoostHook20");
_LIT(KPbHookFile, "PBHOOK20.LDD");          // \System\Libs\ on any drive
const TInt KPbHookMajor = 0;
const TInt KPbHookMinor = 20;
const TInt KPbHookBuild = 1;
const TInt KPbHookMagic = 0x4842;           // "HB"

enum TPbHookControl
	{
	EPbVersion = 0,      // -> KPbHookMagic<<16 | major<<8 | minor
	EPbCpuMode = 1,      // -> CPSR & 0xff
	EPbUbrcrRead = 2,    // -> UART2's UBRCR, low 16 bits (read only)
	EPbInstall = 3,      // a1 = the Wind2 factory -> 0, KErrAlreadyExists (ours already),
	                     // KErrNotFound (not the stock factory), KErrCorrupt (ROM slots differ)
	EPbRemove = 4,       // a1 = the factory -> 0, or KErrNotReady (not ours)
	EPbStatus = 6,       // a1 = the factory -> 1 hooked by this driver, 0 not
	EPbPeekLow = 7,      // a1 = a ROM or kernel-data word -> its low half (latched)
	EPbPeekHigh = 8,     // -> the latched word's high half
	EPbVtCopy = 9        // -> our factory vtable copy's address, low half (then EPbPeekHigh)
	};

// The 5mx ROM 1.05(260)'s addresses (docs/how-it-works.md)
const TUint32 KPbRomLow = 0x50000000, KPbRomHigh = 0x51000000;
const TUint32 KPbKDatLow = 0x80000000, KPbKDatHigh = 0x80400000;
const TUint32 KPbUbrcr = 0x58000708;            // virtual UART2 UBRCR
const TUint32 KPbWind2Factory = 0x5008b61c;     // "Comm.Wind2" DPhysicalDevice vtable
const TUint32 KPbWind1Factory = 0x5008a864;     // "Comm.Wind1" (IrDA): never touched
const TUint32 KPbWind2ChanVtable = 0x5008b5dc;  // its DComm channel vtable
const TUint32 KPbWind2ConfigFn = 0x5008aedc;    // channel vtable +0x28: programs the baud
const TUint32 KPbWind2CreateFn = 0x5008aafc;    // factory vtable +0x34: makes a DComm channel
const TInt KPbChanVtWords = 16;
const TInt KPbFacVtWords = 14;
const TInt KPbConfigSlot = 10;                  // +0x28 / 4
const TInt KPbCreateSlot = 13;                  // +0x34 / 4
const TUint32 KPbPddContainer = 0x80000998;     // the PDD DObjectCon pointer
const TInt KPbConArrayOff = 0x08, KPbArrCountOff = 0x04, KPbArrObjsOff = 0x08;
const TInt KPbObjNameOff = 0x10;                // DObject's iName (an HBufC8)
const TInt KPbRomMajor = 1, KPbRomMinor = 5, KPbRomBuild = 260;

class RPbHook : public RBusLogicalChannel
	{
public:
	inline TInt Open()
		{ return DoCreate(KPbHookName, TVersion(KPbHookMajor, KPbHookMinor, KPbHookBuild), NULL, KNullUnit, NULL, NULL); }
	inline TInt Version() { return DoControl(EPbVersion); }
	inline TInt UbrcrRead() { return DoControl(EPbUbrcrRead); }
	inline TInt Install(TUint32 aFactory) { return DoControl(EPbInstall, (TAny*)aFactory); }
	inline TInt Remove(TUint32 aFactory) { return DoControl(EPbRemove, (TAny*)aFactory); }
	inline TInt Status(TUint32 aFactory) { return DoControl(EPbStatus, (TAny*)aFactory); }
	inline TInt VtCopy(TUint32& aAddr)
		{
		TInt lo = DoControl(EPbVtCopy);
		if (lo < 0) return lo;
		TInt hi = DoControl(EPbPeekHigh);
		if (hi < 0) return hi;
		aAddr = ((TUint32)hi << 16) | (TUint32)lo;
		return KErrNone;
		}
	// a guarded ROM or kernel-data word read (the driver refuses anything else)
	inline TInt Peek(TUint32 aAddr, TUint32& aValue)
		{
		TInt lo = DoControl(EPbPeekLow, (TAny*)aAddr);
		if (lo < 0) return lo;
		TInt hi = DoControl(EPbPeekHigh);
		if (hi < 0) return hi;
		aValue = ((TUint32)hi << 16) | (TUint32)lo;
		return KErrNone;
		}
	};

// What PbFindFactory found
enum TPbFound
	{
	EPbNoFactory,        // no "Comm.Wind2" factory (or the container is not as expected)
	EPbStock,            // the factory, not hooked
	EPbHooked,           // the factory, hooked by this driver
	EPbForeign           // the factory, its vtable changed by someone else (another
	                     // hook driver, or an older PsiBoost's): restart the Psion
	};

inline TBool PbInKData(TUint32 a) { return a >= KPbKDatLow && a < KPbKDatHigh && !(a & 3); }

// Walks the kernel's PDD container for the RS-232 port's factory, with
// bounded, guarded reads only. aFactory gets its address (EPbStock,
// EPbHooked, EPbForeign). The serial PDDs are loaded first, so that the
// factory exists (at boot nothing may have loaded them yet).
inline TInt PbFindFactory(RPbHook& aHook, TUint32& aFactory)
	{
	aFactory = 0;
	User::LoadPhysicalDevice(_L("EUART1"));
	User::LoadPhysicalDevice(_L("EUART2"));
	TUint32 copy = 0;
	if (aHook.VtCopy(copy) != KErrNone)
		copy = 0;
	TUint32 con = 0, arr = 0, count = 0, objs = 0;
	if (aHook.Peek(KPbPddContainer, con) != KErrNone || !PbInKData(con))
		return EPbNoFactory;
	if (aHook.Peek(con + KPbConArrayOff, arr) != KErrNone || !PbInKData(arr))
		return EPbNoFactory;
	if (aHook.Peek(arr + KPbArrCountOff, count) != KErrNone || aHook.Peek(arr + KPbArrObjsOff, objs) != KErrNone)
		return EPbNoFactory;
	if (count < 1 || count > 64 || !PbInKData(objs))
		return EPbNoFactory;
	TUint32 foreign = 0;
	for (TUint i = 0; i < count; i++)
		{
		TUint32 obj = 0, vptr = 0;
		if (aHook.Peek(objs + i * 4, obj) != KErrNone || !PbInKData(obj))
			continue;
		if (aHook.Peek(obj, vptr) != KErrNone)
			continue;
		if (vptr == KPbWind2Factory)
			{
			aFactory = obj;
			return EPbStock;
			}
		if (copy && vptr == copy)
			{
			aFactory = obj;
			return EPbHooked;
			}
		if (vptr == KPbWind1Factory || foreign)
			continue;
		// another vtable: is it the Wind2 factory by name ("Comm.Wind2", 8-bit,
		// after the HBufC's length word)?
		TUint32 name = 0, w0 = 0, w1 = 0, w2 = 0;
		if (aHook.Peek(obj + KPbObjNameOff, name) != KErrNone || !PbInKData(name))
			continue;
		if (aHook.Peek(name + 4, w0) != KErrNone || aHook.Peek(name + 8, w1) != KErrNone || aHook.Peek(name + 12, w2) != KErrNone)
			continue;
		// "Comm" ".Win" "d2"
		if (w0 == 0x6d6d6f43 && w1 == 0x6e69572e && (w2 & 0xffff) == 0x3264)
			foreign = obj;
		}
	if (foreign)
		{
		aFactory = foreign;
		return EPbForeign;
		}
	return EPbNoFactory;
	}

#endif
