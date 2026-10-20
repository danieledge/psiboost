// pbuart.cpp - PBUART21.LDD, PsiBoost's UART driver for the Psion Series 5mx
// (EPOC R5, ROM 1.05(260)). See ../pbuart.h and ../docs/how-it-works.md.
//
// Kernel code: a bug here takes the whole machine down, and C: is a RAM
// disk. What it does, all from DoControl:
//  - EPuVersion, EPuCpuMode: the driver is loaded and runs in a privileged mode;
//  - EPuRead32/EPuRead8/EPuLatchHigh: reads UART2's registers, from a fixed
//    list with no read side effects (no data or FIFO register);
//  - EPuUbrcrSet: the one write, UART2's baud divider (UBRCR), 1 or 3 only,
//    and only while the UART is on. The value found is saved first and put
//    back by EPuRestore and when the channel closes, if the register still
//    holds what was written.
// It never touches user memory: every answer is DoControl's return value.
#include "kern.h"
#include <e32svr.h>
#include "pbuart.h"

const TUint32 KRegBase = 0x58000000;     // the kernel's own "mov rX,#0x58000000"
const TInt KUartEnable = 0x01;           // UART2 CON: UARTEN

// readable registers: offset and access width (4 or 1)
struct TRegOk { TUint16 iOffset; TUint16 iWidth; };
static const TRegOk KReadable[] =
	{
	{EPuRegFcr, 4}, {EPuRegUbrcr, 4}, {EPuRegCon, 1}, {EPuRegFlg, 1}, {EPuRegIntM, 1}
	};

static TBool Readable(TInt aOffset, TInt aWidth)
	{
	for (TUint i = 0; i < sizeof(KReadable) / sizeof(KReadable[0]); i++)
		if (KReadable[i].iOffset == aOffset && KReadable[i].iWidth == aWidth)
			return ETrue;
	return EFalse;
	}

static TUint CpuMode()
	{
	TUint cpsr;
	asm volatile("mrs %0, cpsr" : "=r"(cpsr));
	return cpsr & 0xff;
	}

class DPbUartDevice : public DLogicalDevice
	{
public:
	DPbUartDevice();
	virtual TInt Install();
	virtual void GetCaps(TDes8& aDes) const;
	virtual DLogicalChannel* CreateL();
	};

class DPbUartChannel : public DLogicalChannel
	{
public:
	DPbUartChannel(DLogicalDevice* aDevice);
	~DPbUartChannel();
protected:
	virtual void DoCancel(TInt aReqNo);
	virtual void DoRequest(TInt aReqNo, TAny* a1, TAny* a2);
	virtual void DoCreateL(TInt aUnit, CBase* aPdd, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt DoControl(TInt aFunction, TAny* a1, TAny* a2);
private:
	void Restore();
	TInt UbrcrSet(TInt aValue);
private:
	TUint32 iLatch;                    // the last EPuRead32
	TBool iUbrcrSet;                   // UBRCR written: iUbrcrSaved to put back
	TUint32 iUbrcrSaved, iUbrcrWritten;
	};

// --- the device

DPbUartDevice::DPbUartDevice()
	{
	iVersion = TVersion(KPbUartMajor, KPbUartMinor, KPbUartBuild);
	iParseMask = 0;     // no unit, no physical device, no info
	iUnitsMask = 0;
	}

TInt DPbUartDevice::Install()
	{
	return SetName(&KPbUartName);
	}

void DPbUartDevice::GetCaps(TDes8& aDes) const
	{
	TVersion v(KPbUartMajor, KPbUartMinor, KPbUartBuild);
	aDes.FillZ(aDes.MaxLength());
	aDes.Copy((const TUint8*)&v, Min(aDes.MaxLength(), (TInt)sizeof(v)));
	}

DLogicalChannel* DPbUartDevice::CreateL()
	{
	return new(ELeave) DPbUartChannel(this);
	}

// --- the channel

DPbUartChannel::DPbUartChannel(DLogicalDevice* aDevice)
	: DLogicalChannel(aDevice)
	{
	}

DPbUartChannel::~DPbUartChannel()
	{
	Restore();
	}

void DPbUartChannel::DoCreateL(TInt /*aUnit*/, CBase* /*aPdd*/, const TDesC8* /*aInfo*/, const TVersion& aVer)
	{
	if (!User::QueryVersionSupported(TVersion(KPbUartMajor, KPbUartMinor, KPbUartBuild), aVer))
		User::Leave(KErrNotSupported);
	}

void DPbUartChannel::DoCancel(TInt /*aReqNo*/)
	{
	}

void DPbUartChannel::DoRequest(TInt /*aReqNo*/, TAny* /*a1*/, TAny* /*a2*/)
	{
	// no asynchronous requests (iBehaviour stays 0, so the kernel refuses them)
	}

TInt DPbUartChannel::DoControl(TInt aFunction, TAny* a1, TAny* /*a2*/)
	{
	TInt off = (TInt)a1;
	switch (aFunction)
		{
	case EPuVersion:
		return (KPbUartMagic << 16) | (KPbUartMajor << 8) | KPbUartMinor;
	case EPuCpuMode:
		return CpuMode();
	case EPuRead32:
		if (!Readable(off, 4))
			return KErrArgument;
		iLatch = *(volatile TUint32*)(KRegBase + off);
		return iLatch & 0xffff;
	case EPuRead8:
		if (!Readable(off, 1))
			return KErrArgument;
		return *(volatile TUint8*)(KRegBase + off);
	case EPuLatchHigh:
		return iLatch >> 16;
	case EPuRestore:
		Restore();
		return KErrNone;
	case EPuUbrcrSet:
		return UbrcrSet(off);
	default:
		return KErrNotSupported;
		}
	}

// UBRCR back as it was found, but only if it still holds what was written:
// a port opened since keeps the divider it was given
void DPbUartChannel::Restore()
	{
	if (!iUbrcrSet)
		return;
	volatile TUint32* ubrcr = (volatile TUint32*)(KRegBase + EPuRegUbrcr);
	if (*ubrcr == iUbrcrWritten)
		*ubrcr = iUbrcrSaved;
	iUbrcrSet = EFalse;
	}

// UBRCR = 7372800 / (16 x rate) - 1: 3 is 115200, 1 is 230400. Only while the
// UART is on (a port is open), and only these two values.
TInt DPbUartChannel::UbrcrSet(TInt aValue)
	{
	if (aValue != 1 && aValue != 3)
		return KErrArgument;
	if (!(*(volatile TUint8*)(KRegBase + EPuRegCon) & KUartEnable))
		return KErrNotReady;
	volatile TUint32* ubrcr = (volatile TUint32*)(KRegBase + EPuRegUbrcr);
	if (!iUbrcrSet)
		{
		iUbrcrSaved = *ubrcr;
		iUbrcrSet = ETrue;
		}
	iUbrcrWritten = (TUint32)aValue;
	*ubrcr = iUbrcrWritten;
	return KErrNone;
	}

// --- the DLL

EXPORT_C DLogicalDevice* CreateLogicalDevice()
	{
	return new DPbUartDevice;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
