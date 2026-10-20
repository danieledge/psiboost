// pbrec.cpp - PsiBoost.rdl: puts PsiBoost's serial-speed hook back at
// start-up. See ../pbhook.h and ../docs/how-it-works.md.
//
// A file recogniser (UID2 0x1000013E) that recognises nothing. Apparc's
// server looks in \System\Recogs\ on every disk with a medium in it, at
// start-up and whenever a file there changes, loads each recogniser and
// calls its ordinal 1 (apparc/apfile/apfrec.cpp in the SDK:
// CApaScanningFileRecognizer::ScanForRecognizersL). Ours does its work there
// and returns NULL, so Apparc unloads it again and none of our code is left
// in the server. The work, all of it checked, none of it able to leave:
//   - nothing at all if D:\System\Data\PsiBoost.off (or C:'s) exists, or if
//     Ctrl is held down;
//   - nothing unless this is a 5mx with ROM 1.05(260);
//   - nothing unless PsiBoost.ini (in \System\Apps\PsiBoost\ on C: or D:)
//     is version 4 or later and says 230400 always is on;
//   - then PBHOOK20.LDD is loaded and opened, the RS-232 port's PDD factory
//     found by guarded reads, and the hook installed by the driver, which
//     checks the factory and the two ROM slots first and writes nothing if
//     any differ (PsiBoost.app does exactly the same).
// One line goes into PsiBoost.log beside the settings, saying what was done.
//
// It is installed on the CF card (D:) only: pull the card and restart, and
// it is gone.
#include <e32std.h>
#include <e32base.h>
#include <e32hal.h>
#include <e32keys.h>
#include <f32file.h>
#include <w32std.h>
#include "pbhook.h"

class CApaFileRecognizerType;

_LIT(KPbAppFolder, "?:\\System\\Apps\\PsiBoost\\");
_LIT(KPbIni, "PsiBoost.ini");
_LIT(KPbLog, "PsiBoost.log");
_LIT(KPbOffD, "D:\\System\\Data\\PsiBoost.off");
_LIT(KPbOffC, "C:\\System\\Data\\PsiBoost.off");
const TInt KPbIniAlwaysVersion = 4;            // PsiBoost.ini: field 0 is 230400 always since version 4

static TBool Exists(RFs& aFs, const TDesC& aName)
	{
	TEntry e;
	return aFs.Entry(aName, e) == KErrNone;
	}

static TInt Int32At(const TUint8* aP)
	{
	return aP[0] | (aP[1] << 8) | (aP[2] << 16) | (aP[3] << 24);
	}

// PsiBoost's folder on C: or D: whose PsiBoost.ini can be read: aOn is its
// 230400 always. EFalse if there is none.
static TBool ReadIni(RFs& aFs, TDes& aFolder, TBool& aOn)
	{
	aOn = EFalse;
	const TText drives[2] = { 'C', 'D' };
	for (TInt i = 0; i < 2; i++)
		{
		aFolder = KPbAppFolder;
		aFolder[0] = drives[i];
		TFileName ini(aFolder);
		ini.Append(KPbIni);
		RFile f;
		if (f.Open(aFs, ini, EFileRead | EFileShareAny) != KErrNone)
			continue;
		TBuf8<12> d;
		TInt r = f.Read(d);
		f.Close();
		if (r != KErrNone || d.Length() < 12)
			continue;
		const TUint8* p = d.Ptr();
		aOn = Int32At(p) >= KPbIniAlwaysVersion && Int32At(p + 4) >= 1 && Int32At(p + 8) == 1;
		return ETrue;
		}
	return EFalse;
	}

static void Log(RFs& aFs, const TDesC& aFolder, const TDesC8& aLine)
	{
	TFileName name(aFolder);
	name.Append(KPbLog);
	RFile f;
	TInt r = f.Open(aFs, name, EFileWrite | EFileShareAny);
	if (r == KErrNotFound)
		r = f.Create(aFs, name, EFileWrite | EFileShareAny);
	if (r != KErrNone)
		return;
	TInt size = 0;
	if (f.Size(size) == KErrNone && size < 32768)
		{
		TInt end = 0;
		f.Seek(ESeekEnd, end);
		TTime t;
		t.HomeTime();
		TDateTime dt = t.DateTime();
		TBuf8<120> l;
		l.Format(_L8("%02d:%02d:%02d Start-up: "), dt.Hour(), dt.Minute(), dt.Second());
		l.Append(aLine.Left(Min(aLine.Length(), 90)));
		l.Append(_L8("\r\n"));
		f.Write(l);
		}
	f.Close();
	}

static TBool CtrlHeld()
	{
	RWsSession ws;
	if (ws.Connect() != KErrNone)
		return EFalse;
	TInt m = ws.GetModifierState();
	ws.Close();
	return (m & EModifierCtrl) != 0;
	}

static TBool RomOk()
	{
	TMachineInfoV1Buf mi;
	if (UserHal::MachineInfo(mi) != KErrNone)
		return EFalse;
	const TVersion& v = mi().iRomVersion;
	return v.iMajor == KPbRomMajor && v.iMinor == KPbRomMinor && v.iBuild == KPbRomBuild
		&& mi().iDisplaySizeInPixels == TSize(640, 240);
	}

// the hook, if PsiBoost.ini says so: what was done, for the log
static void PutHookBack(RFs& aFs, const TDesC& aFolder)
	{
	TBuf8<90> m;
	TInt r = User::LoadLogicalDevice(KPbHookFile);
	if (r != KErrNone && r != KErrAlreadyExists)
		{
		m.Format(_L8("230,400 bps not turned on - its driver did not load (%d)"), r);
		Log(aFs, aFolder, m);
		return;
		}
	RPbHook hook;
	r = hook.Open();
	if (r != KErrNone)
		{
		m.Format(_L8("230,400 bps not turned on - its driver did not open (%d)"), r);
		Log(aFs, aFolder, m);
		return;
		}
	if (((TUint)hook.Version() >> 16) != (TUint)KPbHookMagic)
		{
		hook.Close();
		Log(aFs, aFolder, _L8("230,400 bps not turned on - its driver is not the expected one"));
		return;
		}
	TUint32 factory = 0;
	TInt found = PbFindFactory(hook, factory);
	if (found == EPbHooked)
		;                                     // (on already: Apparc looks again when a disk changes; nothing to say)
	else if (found == EPbStock)
		{
		r = hook.Install(factory);
		if (r == KErrNone)
			m = _L8("230,400 bps always is on again");
		else
			m.Format(_L8("230,400 bps not turned on (%d) - nothing was changed"), r);
		}
	else if (found == EPbForeign)
		m = _L8("230,400 bps not turned on - another program has changed the serial port");
	else
		m = _L8("the serial port's driver was not found: nothing done");
	hook.Close();
	if (m.Length())
		Log(aFs, aFolder, m);
	}

static void Run()
	{
	RFs fs;
	if (fs.Connect() != KErrNone)
		return;
	TFileName folder;
	TBool on = EFalse;
	if (Exists(fs, KPbOffD) || Exists(fs, KPbOffC))
		{
		if (ReadIni(fs, folder, on))
			Log(fs, folder, _L8("PsiBoost.off is there: nothing done"));
		}
	else if (!RomOk())
		;                                     // (another machine or ROM: nothing, not even the log)
	else if (CtrlHeld())
		{
		if (ReadIni(fs, folder, on))
			Log(fs, folder, _L8("Ctrl was held down: nothing done"));
		}
	else if (ReadIni(fs, folder, on) && on)
		PutHookBack(fs, folder);
	fs.Close();
	}

// ordinal 1: Apparc's gate. NULL: no recogniser to keep (Apparc unloads us)
EXPORT_C CApaFileRecognizerType* CreateRecognizer()
	{
	Run();
	return NULL;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
