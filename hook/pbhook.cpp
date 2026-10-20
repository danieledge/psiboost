// pbhook.cpp - PBHOOK20.LDD, PsiBoost's serial-speed hook for the Psion
// Series 5mx (EPOC R5, ROM 1.05(260)). See ../pbhook.h and
// ../docs/how-it-works.md. Kernel code: a bug here takes the whole machine
// down. Tried on a real 5mx in October 2026.
//
// It changes nothing until EPbInstall. With no writable static data (an EPOC
// DLL may have none), the two patched vtables are constant arrays, as real
// vtables are: the stock ROM slots as literal addresses, one slot replaced
// by one of our functions. The only state is the one word in the serial PDD
// factory object, which EPbInstall points at our factory-vtable copy and
// EPbRemove points back; EPbStatus reads it.
//
// Channel vtable (0x5008b5dc, 16 words): its configure slot (+0x28, index
// 10) programs the baud; ours is ConfigWrap. Factory vtable (0x5008b61c, 14
// words): its create slot (+0x34, index 13) makes a DComm channel; ours,
// CreateWrap, points each new channel at our channel-vtable copy.
#include "kern.h"
#include <e32svr.h>
#include "pbhook.h"

static TUint32 CreateWrap(TUint32 aThis, TUint32 a1, TUint32 a2, TUint32 a3);
static TInt ConfigWrap(TUint32 aThis, TUint32 aCfg);

static const TUint32 KChanVtCopy[KPbChanVtWords] =
	{
	0x00000000, 0x00000000, 0x5008afac, 0x5008ac3c,
	0x5008ac78, 0x5008ad08, 0x5008ad20, 0x5008ad98,
	0x5008adf0, 0x5008ae2c, (TUint32)&ConfigWrap, 0x5008afa4,   // index 10 -> ours
	0x5008aeb0, 0x5008afdc, 0x5008afec, 0x5008aeb4,
	};
static const TUint32 KFacVtCopy[KPbFacVtWords] =
	{
	0x00000000, 0x00000000, 0x5008b460, 0x5008b2c8,
	0x5008b2d4, 0x5008b2e0, 0x5008b2ec, 0x5008aa3c,
	0x5008b40c, 0x5008b418, 0x5008b424, 0x5008aa04,
	0x5008aaf4, (TUint32)&CreateWrap,                             // index 13 -> ours
	};

typedef TUint32 (*TCreateFn)(TUint32, TUint32, TUint32, TUint32);
typedef TInt (*TConfigFn)(TUint32, TUint32);

// the channel made the stock way, then pointed at our channel vtable copy
static TUint32 CreateWrap(TUint32 aThis, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	TUint32 ch = ((TCreateFn)KPbWind2CreateFn)(aThis, a1, a2, a3);
	if (ch >= KPbKDatLow && ch < KPbKDatHigh)
		*(volatile TUint32*)ch = (TUint32)&KChanVtCopy[0];
	return ch;
	}

// The ROM programs the baud; if it wrote the 115200 divider (3), 230400 (1).
// Every other rate is left as it was. (UBRCR reads back with other bits above
// the divider, such as 0x18170003 on the 5mx: only the low byte is compared.
// Writing 1 is what PBUART21.LDD's UbrcrSet does.)
static TInt ConfigWrap(TUint32 aThis, TUint32 aCfg)
	{
	TInt r = ((TConfigFn)KPbWind2ConfigFn)(aThis, aCfg);
	volatile TUint32* u = (volatile TUint32*)KPbUbrcr;
	if ((*u & 0xff) == 3)
		*u = 1;
	return r;
	}

static TUint CpuMode()
	{
	TUint cpsr;
	asm volatile("mrs %0, cpsr" : "=r"(cpsr));
	return cpsr & 0xff;
	}

static TBool Peekable(TUint32 a)
	{
	if (a & 3)
		return EFalse;
	return (a >= KPbRomLow && a < KPbRomHigh) || (a >= KPbKDatLow && a < KPbKDatHigh);
	}

// The factory pointed at our vtable copy: nothing is written unless the
// factory and the two ROM slots are exactly what this build expects.
static TInt Install(TUint32 aFactory)
	{
	if (aFactory < KPbKDatLow || aFactory >= KPbKDatHigh || (aFactory & 3))
		return KErrArgument;
	TUint32 vptr = *(volatile TUint32*)aFactory;
	if (vptr == (TUint32)&KFacVtCopy[0])
		return KErrAlreadyExists;
	if (vptr != KPbWind2Factory)
		return KErrNotFound;
	if (*(volatile TUint32*)(KPbWind2ChanVtable + KPbConfigSlot * 4) != KPbWind2ConfigFn)
		return KErrCorrupt;
	if (*(volatile TUint32*)(KPbWind2Factory + KPbCreateSlot * 4) != KPbWind2CreateFn)
		return KErrCorrupt;
	*(volatile TUint32*)aFactory = (TUint32)&KFacVtCopy[0];     // the hook is live
	return KErrNone;
	}

static TInt Remove(TUint32 aFactory)
	{
	if (aFactory < KPbKDatLow || aFactory >= KPbKDatHigh || (aFactory & 3))
		return KErrArgument;
	if (*(volatile TUint32*)aFactory != (TUint32)&KFacVtCopy[0])
		return KErrNotReady;
	*(volatile TUint32*)aFactory = KPbWind2Factory;
	return KErrNone;
	}

class DPbHookDevice : public DLogicalDevice
	{
public:
	DPbHookDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual DLogicalChannel* CreateL();
	};

class DPbHookChannel : public DLogicalChannel
	{
public:
	DPbHookChannel(DLogicalDevice* aDevice) : DLogicalChannel(aDevice) {}
protected:
	virtual void DoCancel(TInt) {}
	virtual void DoRequest(TInt, TAny*, TAny*) {}
	virtual void DoCreateL(TInt, CBase*, const TDesC8*, const TVersion& aVer);
	virtual TInt DoControl(TInt aFunction, TAny* a1, TAny* a2);
private:
	TUint32 iPeek;
	};

DPbHookDevice::DPbHookDevice()
	{
	iVersion = TVersion(KPbHookMajor, KPbHookMinor, KPbHookBuild);
	iParseMask = 0;
	iUnitsMask = 0;
	}

TInt DPbHookDevice::Install()
	{
	return SetName(&KPbHookName);
	}

void DPbHookDevice::GetCaps(TDes8& aDes) const
	{
	TVersion v(KPbHookMajor, KPbHookMinor, KPbHookBuild);
	aDes.FillZ(aDes.MaxLength());
	aDes.Copy((const TUint8*)&v, Min(aDes.MaxLength(), (TInt)sizeof(v)));
	}

DLogicalChannel* DPbHookDevice::CreateL()
	{
	return new(ELeave) DPbHookChannel(this);
	}

void DPbHookChannel::DoCreateL(TInt, CBase*, const TDesC8*, const TVersion& aVer)
	{
	if (!User::QueryVersionSupported(TVersion(KPbHookMajor, KPbHookMinor, KPbHookBuild), aVer))
		User::Leave(KErrNotSupported);
	}

TInt DPbHookChannel::DoControl(TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	switch (aFunction)
		{
	case EPbVersion:
		return (KPbHookMagic << 16) | (KPbHookMajor << 8) | KPbHookMinor;
	case EPbCpuMode:
		return CpuMode();
	case EPbUbrcrRead:
		return (TInt)(*(volatile TUint32*)KPbUbrcr & 0xffff);
	case EPbInstall:
		return Install((TUint32)a1);
	case EPbRemove:
		return Remove((TUint32)a1);
	case EPbStatus:
		{
		TUint32 f = (TUint32)a1;
		if (f < KPbKDatLow || f >= KPbKDatHigh || (f & 3))
			return KErrArgument;
		return *(volatile TUint32*)f == (TUint32)&KFacVtCopy[0] ? 1 : 0;
		}
	case EPbPeekLow:
		{
		TUint32 a = (TUint32)a1;
		if (!Peekable(a))
			return KErrArgument;
		iPeek = *(volatile TUint32*)a;
		return iPeek & 0xffff;
		}
	case EPbPeekHigh:
		return iPeek >> 16;
	case EPbVtCopy:
		iPeek = (TUint32)&KFacVtCopy[0];
		return iPeek & 0xffff;
	default:
		return KErrNotSupported;
		}
	}

EXPORT_C DLogicalDevice* CreateLogicalDevice()
	{
	return new DPbHookDevice;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
