// PBCTL.CPP - PsiBoost.ctl: PsiBoost in the Control panel (0.27).
//
// The Control panel loads this DLL and calls its first export with the
// DLL's own name, in the System screen's thread (or PsiHub's). It shows
// PsiBoost's two settings, 230,400 bps always and the other end, as they are
// in PsiBoost.ini, and the last speed test. The work itself (telling the
// modem, putting the hook in, the test) takes a while and needs PsiBoost's
// helper, so it stays in PsiBoost.app: OK with a change, Test speed, Free
// port and Details open PsiBoost with a command (its tail, or a window server
// message if it is open already), and PsiBoost does it with its busy message
// and says how it went. PsiBoost.app is hidden from the Extras bar since
// 0.27: this item is the way in.

#include <e32std.h>
#include <f32file.h>
#include <eikenv.h>
#include <eikdialg.h>
#include <eikchlst.h>
#include <eiklabel.h>
#include <eikdll.h>
#include <apgtask.h>
#include <apacmdln.h>
#include <pbctl.rsg>
#include "pbctl.hrh"
#include "pbcmd.h"

_LIT(KPbApp, "PsiBoost.app");
_LIT(KPbDir, "\\System\\Apps\\PsiBoost\\");
_LIT(KPbIni, "PsiBoost.ini");

// what the dialog shows, from PsiBoost.ini (psiboost.cpp's LoadSettings):
// version, count, then 32-bit fields (0 230400 always, 2 the other end,
// 8 a test done), then two strings (your own server, the last verdict)
struct TPbState
	{
	TInt iAlways;
	TInt iEnd;
	TInt iTested;
	TBuf<80> iVerdict;
	};

static TInt Int32At(const TUint8* aP)
	{
	return aP[0] | (aP[1] << 8) | (aP[2] << 16) | (aP[3] << 24);
	}

static void LoadState(RFs& aFs, const TDesC& aDir, TPbState& aState)
	{
	aState.iAlways = aState.iEnd = aState.iTested = 0;
	aState.iVerdict.Zero();
	TFileName ini(aDir);
	ini.Append(KPbIni);
	RFile f;
	if (f.Open(aFs, ini, EFileRead | EFileShareReadersOnly) != KErrNone)
		return;
	HBufC8* buf = HBufC8::New(8 + 4 * 32 + 1 + 60 + 1 + 200);
	if (!buf)
		{
		f.Close();
		return;
		}
	TPtr8 d(buf->Des());
	TInt r = f.Read(d);
	f.Close();
	const TUint8* p = d.Ptr();
	if (r == KErrNone && d.Length() >= 8)
		{
		TInt version = Int32At(p), count = Int32At(p + 4);
		if (version >= 4 && count >= 9 && count <= 32 && d.Length() >= 8 + 4 * count)
			{
			aState.iAlways = Int32At(p + 8) == 1;
			aState.iEnd = Int32At(p + 8 + 2 * 4) == 1;
			aState.iTested = Int32At(p + 8 + 8 * 4) != 0;
			TInt sp = 8 + 4 * count;
			if (sp < d.Length())
				{
				sp += 1 + p[sp];                        // (your own server)
				if (sp < d.Length() && sp + 1 + p[sp] <= d.Length())
					{
					TPtrC8 v(d.Mid(sp + 1, p[sp]));
					aState.iVerdict.Copy(v.Left(Min(v.Length(), aState.iVerdict.MaxLength())));
					}
				}
			}
		}
	delete buf;
	}

class CPbDialog : public CEikDialog
	{
public:
	CPbDialog(TPbState& aState, TInt& aButton) : iState(aState), iButton(aButton) {}
private:
	void PreLayoutDynInitL()
		{
		((CEikChoiceListBase*)Control(EPbCtlAlways))->SetCurrentItem(iState.iAlways ? 1 : 0);
		((CEikChoiceListBase*)Control(EPbCtlEnd))->SetCurrentItem(iState.iEnd ? 1 : 0);
		NoteL(iState.iEnd);
		TBuf<100> last;
		if (iState.iTested && iState.iVerdict.Length())
			last.Format(_L("Last speed test: %S"), &iState.iVerdict);
		else
			last.Copy(_L("No speed test yet"));
		((CEikLabel*)Control(EPbCtlLast))->SetTextL(last);
		}
	void HandleControlStateChangeL(TInt aControlId)
		{
		if (aControlId == EPbCtlEnd)
			{
			NoteL(((CEikChoiceListBase*)Control(EPbCtlEnd))->CurrentItem());
			Control(EPbCtlNote)->DrawNow();
			}
		}
	void NoteL(TInt aEnd)
		{
		((CEikLabel*)Control(EPbCtlNote))->SetTextL(aEnd
			? _L("Programs asking for 115,200 bps get 230,400: set the computer's port to match.")
			: _L("Programs asking for 115,200 bps get 230,400: PsiBoost tells the modem."));
		}
	TBool OkToExitL(TInt aButtonId)
		{
		iState.iAlways = ((CEikChoiceListBase*)Control(EPbCtlAlways))->CurrentItem() == 1;
		iState.iEnd = ((CEikChoiceListBase*)Control(EPbCtlEnd))->CurrentItem() == 1;
		iButton = aButtonId;
		return ETrue;
		}
	TPbState& iState;
	TInt& iButton;
	};

// PsiBoost given aCommand (pbcmd.h): told, if it is open; else started with it
static void TellPsiBoostL(CEikonEnv* aEnv, const TDesC& aApp, TUint aCommand)
	{
	RWsSession& ws = aEnv->WsSession();
	TApaTaskList tasks(ws);
	TApaTask t = tasks.FindApp(KUidPbApp);
	if (t.Exists())
		{
		TWsEvent ev;
		ev.SetType(EEventUser);
		((TUint32*)ev.EventData())[0] = KPbCmdEvent;
		((TUint32*)ev.EventData())[1] = aCommand;
		ws.SendEventToWindowGroup(t.WgId(), ev);
		t.BringToForeground();
		return;
		}
	TBuf8<16> tail;
	PbCmdTail(tail, aCommand);
	CApaCommandLine* cmd = CApaCommandLine::NewLC();
	cmd->SetLibraryNameL(aApp);
	cmd->SetCommandL(EApaCommandRun);
	cmd->SetTailEndL(tail);
	EikDll::StartAppL(*cmd);
	CleanupStack::PopAndDestroy();          // cmd
	}

static void RunL(const TDesC& aDllName)
	{
	CEikonEnv* env = CEikonEnv::Static();
	RFs& fs = env->FsSession();
	// PsiBoost.app, on whichever disk it was installed to (on the heap:
	// the System screen's stack is deep here)
	TFindFile* ff = new(ELeave) TFindFile(fs);
	TInt found = ff->FindByDir(KPbApp, KPbDir);
	HBufC* app = found == KErrNone ? ff->File().Alloc() : NULL;
	delete ff;
	if (!app)
		{
		env->InfoMsg(_L("Not available - PsiBoost is not installed"));
		return;
		}
	CleanupStack::PushL(app);
	TParse rsc;
	rsc.Set(_L(".rsc"), &aDllName, NULL);
	TInt offset = env->AddResourceFileL(rsc.FullName());
	TRAPD(err,
		{
		TParsePtrC dir(*app);
		TPbState* state = new(ELeave) TPbState;
		CleanupStack::PushL(state);
		LoadState(fs, dir.DriveAndPath(), *state);
		TPbState was = *state;
		TInt button = 0;
		CEikDialog* d = new(ELeave) CPbDialog(*state, button);
		if (d->ExecuteLD(R_PBCTL_DIALOG))
			{
			TUint command = 0;
			if (button == EPbCtlTest)
				command = EPbCmdTest;
			else if (button == EPbCtlFree)
				command = EPbCmdFree;
			else if (button == EPbCtlOpen)
				command = EPbCmdShow;
			// (OK, and the other buttons too: a change is made first)
			if (state->iEnd != was.iEnd)
				command |= state->iEnd ? EPbCmdComputer : EPbCmdModem;
			if (state->iAlways != was.iAlways)
				command |= state->iAlways ? EPbCmdOn : EPbCmdOff;
			if (command)
				TellPsiBoostL(env, *app, command);
			}
		CleanupStack::PopAndDestroy();      // state
		});
	env->DeleteResourceFile(offset);
	CleanupStack::PopAndDestroy();          // app
	User::LeaveIfError(err);
	}

// the Control panel's entry point: ordinal 1
EXPORT_C TInt CreatePsiBoostControl(const TDesC& aDllName)
	{
	TRAPD(err, RunL(aDllName));
	if (err != KErrNone)
		CEikonEnv::Static()->HandleError(err);
	return err;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
