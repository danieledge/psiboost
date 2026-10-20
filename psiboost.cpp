// PSIBOOST.CPP - PsiBoost.app: every program's serial port at 230400 instead
// of 115200 ("230400 always"). See spshared.h, pbhook.h, README.md and
// docs/how-it-works.md.
//
// One view (CSpView), PsiBoost's page: at the top, in large type, whether
// 230400 always is on, then the hook as it is now, then the last thing that
// happened; under it three big choices: 230400 always (on or off), Free the
// serial port, and Test the speed (its last result). After a test the page
// shows the results side by side (115200, 230400, the gain) until a key or a
// tap. Tools > Show log shows the end of PsiBoost.log. The first time
// PsiBoost opens (or the first time 0.24 opens, if the other end is not
// known yet) it welcomes the user, says what it has been tested on, and asks
// what is on the other end of the serial cable (WelcomeL).
// The serial work is pboostd.exe's (the helper): the app starts it,
// passes it requests through the shared chunk, waits with a busy message and
// shows what it says. The app reads the hook itself (PBHOOK20.LDD), and puts
// it back when it is off but 230400 always is on (a restart without the
// recogniser on the CF card).
#include <e32keys.h>
#include <e32hal.h>
#include <e32svr.h>
#include <eikenv.h>
#include <eikappui.h>
#include <eikapp.h>
#include <eikdoc.h>
#include <eikdialg.h>
#include <eikchlst.h>
#include <eikmfne.h>
#include <eiklabel.h>
#include <eikrted.h>
#include <eiktbar.h>
#include <eikcmbut.h>
#include <eikmenup.h>
#include <eikcmds.hrh>
#include <eikmenu.hrh>
#include <eikdialg.hrh>
#include <txtrich.h>
#include <frmtview.h>
#include <coecntrl.h>
#include <f32file.h>
#include <eikon.rsg>
#include <eikimage.h>
#include <psiboost.rsg>
#include "psiboost.hrh"
#include "spshared.h"
#include "pbhook.h"
#include "spicons.h"                     // (made by mktbar.py: PsiBoost.mbm's pictures)
#define PSILINK_STRUCT_ONLY
#include "psilink.h"                     // (psion-core: Connection settings)

_LIT(KVersion, "0.24");                  // also psiboost.pkg and pboostd.cpp
_LIT8(KVersion8, "0.24");
const TUid KUidPsiBoost = { 0x01000A94 };
_LIT(KIniName, "PsiBoost.ini");
_LIT(KLogName, "PsiBoost.log");
_LIT(KOldLogName, "PsiBoost.old");   // (the helper moves a full log here)
const TInt KLogViewLines = 7;            // (the lines psiboost.rss's r_sp_log_dialog shows)
const TInt KShowLogBytes = 6000;         // Tools > Show log: the end of the log, at most this much
_LIT(KHelperMatch, "pboostd*");
const TInt KTickUs = 500000;             // the view looks at the helper twice a second
const TInt KHookTicks = 4;               // ...and at the hook every 2 s
const TInt KHelperQuitWaitMs = 8000;     // an older helper asked to quit: how long it is given
const TInt KTileCount = 3;
enum TSpTile { ESpTileAlways, ESpTileFree, ESpTileTest };
typedef TBuf<24> TSpCell;                // one of the results page's figures (never a TDes*: the stride)
const TInt KResRows = 6;                 // download, echo, round trip, stalls, bad bytes, (0.24) when
const TInt KResCells = KResRows * 3;

// ----- settings: PsiBoost.ini, a version, a count, then 32-bit fields, then
// strings (a length byte and the characters: your own server, the last verdict).
// PsiBoost.rdl reads the first three words: version 4 on, field 0 is 230400
// always.
struct TSpSettings
	{
	TInt iAlways;                        // 230400 always (the hook in, the modem at 230400 and saved)
	TInt iWarned;                        // (0.21) the first-time warning: 0 not shown, 1 shown (0.21 to 0.23), 2 (0.24) the welcome and the other end chosen
	TInt iOtherEnd;                      // (0.23) the other end of the cable: 0 a modem, 1 a computer or other device (no AT)
	TInt iToolbar;                       // View > Show toolbar
	TInt iTestRoute;                     // the link test: 0 the modem route, 1 Psion Internet (-1: as Connection settings)
	TInt iTestServer;                    // ...TSpTestServer
	TInt iTestLen;                       // ...0 quick, 1 5 minutes, 2 15 minutes
	TInt iTestBoth;                      // (0.24) 2: the test was last run at both speeds (else one: as 230400 always says). (0.10 to 0.19: Say when the speed changes, 0 or 1)
	TInt iResDone, iResPass;             // the last test's results (0: none yet); iResPass bit 0 the verdict passed, (0.24) bit 1 the 115200 column, bit 2 the 230400 column
	TInt iRes[2][6];                     // ...at 115200 and 230400: down, echo, rtt, stalls, bad, drops (down -1: not run)
	TInt iResAt[2];                      // (0.24) when each column was run: seconds since 1 Jan 2000, local time (0: not known). (0.19: the last call's time, and whether it opened)
	TBuf8<60> iTestHost;                 // the test's own server: "host" or "host:port"
	TBuf8<200> iVerdict;                 // ...and its last verdict line
	};
const TInt KSpFields = 24;               // the 32-bit fields above
// (1: 0.10 to 0.17, nine fields and the host; 2: 0.18, 22 fields; 3: 0.19,
// field 0 was 230400 for Psion Internet calls, AT$CS; 4: 0.20, 230400 always;
// 0.21 uses field 1, once spare, for the warning; 0.23 field 2 for the other end;
// 0.24 field 7 for the test's speeds, 22 and 23 for when each column was run,
// and bits 1 and 2 of field 9)
const TInt KSpIniVersion = 4;
const TInt KTestSecs[3] = { 15, 75, 225 };   // each leg (four legs: two at each speed)

// what the helper is doing for the app (one at a time)
enum TSpBusy { ESpIdle, ESpBusySetup, ESpBusyFree, ESpBusyLog };

class CSpAppUi;

// ----- the view -------------------------------------------------------------------
class CSpView : public CCoeControl
	{
public:
	CSpView(CSpAppUi& aUi) : iUi(aUi), iPressed(-1), iDown(-1), iResults(EFalse) {}
	~CSpView();
	void ConstructL(const TRect& aRect);
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
	void ShowResults(TBool aOn);
	TBool Results() const { return iResults; }
private:
	void Draw(const TRect& aRect) const;
	void DrawTile(CWindowGc& aGc, TInt aTile, TInt aLines) const;
	void DrawStatus(CWindowGc& aGc) const;
	void DrawResults(CWindowGc& aGc) const;
	TInt TilesTop() const;
	TInt StateLines() const;
	TRect TileRect(TInt aTile) const;
	TInt TileAt(const TPoint& aPoint) const;
	void InvalidateTile(TInt aTile);
	void SetFocusTile(TInt aTile);
	void LoadPicturesL();
	void HandlePointerEventL(const TPointerEvent& aEvent);
	CSpAppUi& iUi;
	CFont* iBig;                   // "230400 always: on"; the results' verdict
	CFont* iName;                  // the choices' names
	CFont* iNow;                   // the hook's line
	CFont* iText;                  // how each choice is; the results' numbers
	CFont* iLastFont;              // the page's third line: the last thing that happened
	CFont* iSmall;                 // the results page's hint
	CFbsBitmap* iPic[KTileCount];
	CFbsBitmap* iMask[KTileCount];
	TInt iFocus;                   // the choice the arrow keys are on
	TInt iPressed;                 // the choice the pen is down on (-1: none)
	TInt iDown;                    // ...and the one it went down on
	TBool iResults;                // the results page is shown instead of the choices
	};

// ----- dialogs -------------------------------------------------------------------
class CSpTestDialog : public CEikDialog
	{
public:
	CSpTestDialog(TSpSettings& aS, TInt& aOnly, TBool aNoModem, TBool aHookIn)
		: iS(aS), iOnly(aOnly), iNoModem(aNoModem), iHookIn(aHookIn) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void HandleControlStateChangeL(TInt aControlId);
	void PrepareForFocusTransitionL();
	void DimHost();
	TInt iBase;                      // the base port as last left
	TSpSettings& iS;
	TInt& iOnly;                     // the speed chosen: 115200, 230400, or 0 both
	TBool iNoModem;                  // the other end is a computer: both is not available
	TBool iHookIn;                   // 230400 always is on (the hook in)
	};

// What is on the other end of the serial cable: a modem, or a computer or
// other device (no AT). The second page of the welcome (aFirst: Continue
// only, and Esc does not get out of it), and Speed > The other end.
class CSpEndDialog : public CEikDialog
	{
public:
	CSpEndDialog(TInt& aEnd, TBool aAlways, TBool aFirst) : iEnd(aEnd), iAlways(aAlways), iFirst(aFirst) {}
private:
	void PreLayoutDynInitL();
	TBool OkToExitL(TInt aButtonId);
	void HandleControlStateChangeL(TInt aControlId);
	void ShowNextL();
	TInt& iEnd;
	TBool iAlways;
	TBool iFirst;
	};

// The welcome's first page: what PsiBoost has been tested on. Continue
// only; Esc does not get out of it.
class CSpWelcomeDialog : public CEikDialog
	{
private:
	TBool OkToExitL(TInt aButtonId);
	};

class CSpLogDialog : public CEikDialog
	{
public:
	CSpLogDialog(HBufC* aText) : iText(aText) {}
	~CSpLogDialog() { delete iText; }
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void ShowEndL();
	HBufC* iText;
	};

class CSpAboutDialog : public CEikDialog
	{
public:
	CSpAboutDialog(const TDesC& aStatus) : iStatus(aStatus) {}
private:
	void PreLayoutDynInitL();
	const TDesC& iStatus;
	};

class CSpHelpDialog : public CEikDialog
	{
private:
	void PreLayoutDynInitL();
	void PostLayoutDynInitL();
	void HandleControlStateChangeL(TInt aControlId);
	void ShowTopicL(TInt aTopic);
	TInt iTopic;
	};

// ----- the app ---------------------------------------------------------------------
class CSpAppUi : public CEikAppUi
	{
public:
	void ConstructL();
	~CSpAppUi();
	TBool HelperUp() const { return iS && iS->iState != ESpStopped && iHelperStarted; }
	void TileState(TInt aTile, TDes& aText, TBool& aOn, TBool& aDimmed) const;
	void TileActL(TInt aTile);
	void StatusTexts(TDes& aAlways, TDes& aHook, TDes& aLast) const;
	void ResultTexts(TDes& aVerdict, TSpCell* aCells, TBool& aPass) const;
	void HandleCommandL(TInt aCommand);
private:
	void DynInitMenuPaneL(TInt aResourceId, CEikMenuPane* aMenuPane);
	void LoadSettings();
	void SaveSettings();
	void OpenChunkL();
	TBool HelperQuitL();
	TInt EnsureHelper();
	void HelperDied();
	void HelperMissing(TInt aErr);
	TBool NotNow() const;
	void StartAlwaysL(TBool aOn);
	void AlwaysDone();
	void StartFreeL();
	void FreeDone();
	void StartCheck();
	TBool NoModem() const { return iSettings.iOtherEnd != 0; }
	void AskNoModemL();
	void OtherEndL();
	void SetOtherEnd(TInt aEnd);
	void AppLog(const TDesC& aLine) const;
	void StartTestL();
	void TestDone();
	void SendLogL();
	void ShowLogL();
	void WelcomeL();
	void RestartL();
	void HelperStopped();
	void OpenHook();
	TBool RefreshHook();
	void HookAtStart();
	void LastStartUp(TDes& aLine) const;
	void SetLast(const TDesC& aText);
	void ButtonStates();
	void ButtonPictureL(TInt aId, TInt aIcon);
	void ReadRoute();
	TRect ViewRect() const;
	static TInt Tick(TAny* aThis);
	void DoTick();
	TFileName AppDir() const;

	CSpView* iView;
	CPeriodic* iTimer;
	RChunk iChunk;
	TBool iChunkOpen;
	TSpShared* iS;
	TSpSettings iSettings;
	RProcess iHelper;                    // the helper's process, to see it end
	TBool iHelperOpen;
	TBool iHelperStarted;
	TUint iSeenSeq, iTicks;
	TBool iTesting;
	TInt iBusy;                          // TSpBusy
	TBool iBusyOn;                       // ...ESpBusySetup: turning 230400 always on
	TBool iChecking;                     // a quick look at the modem is asked for
	TBool iWarnPending;                  // the welcome is to be shown (or iWarned brought up to date)
	TInt iTestOnly;                      // the test running: at 115200 or 230400 only, or 0 both
	// the hook, as the app reads it
	RPbHook iHook;
	TBool iHookOpen;
	TInt iHookErr;                       // KErrNone, or why the hook's driver cannot be used
	TInt iHookFound;                     // TPbFound
	TUint32 iFactory;
	TBuf<100> iLast;                     // the page's last line
public:
	TInt iRoute, iBaudIndex;             // Connection settings, as the app read them (-1: not read)
	};

class CSpDocument : public CEikDocument
	{
public:
	CSpDocument(CEikApplication& aApp) : CEikDocument(aApp) {}
private:
	CEikAppUi* CreateAppUiL() { return new(ELeave) CSpAppUi; }
	};

class CSpApplication : public CEikApplication
	{
private:
	TUid AppDllUid() const { return KUidPsiBoost; }
	CApaDocument* CreateDocumentL() { return new(ELeave) CSpDocument(*this); }
	};

// ===========================================================================
// The view
// ===========================================================================

static CFont* FontL(CCoeEnv* aEnv, TInt aTwips, TBool aBold)
	{
	TFontSpec spec(_L("Arial"), aTwips);
	if (aBold)
		spec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	CFont* f = NULL;
	User::LeaveIfError(aEnv->ScreenDevice()->GetNearestFontInTwips(f, spec));
	return f;
	}

void CSpView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	EnableDragEvents();                  // (the pen leaving a choice lets it go)
	Window().SetPointerGrab(ETrue);      // (...and its up comes here)
	iBig = FontL(iCoeEnv, 250, ETrue);
	iName = FontL(iCoeEnv, 200, ETrue);
	iNow = FontL(iCoeEnv, 200, EFalse);
	iText = FontL(iCoeEnv, 150, EFalse);
	iLastFont = FontL(iCoeEnv, 170, EFalse);
	iSmall = FontL(iCoeEnv, 120, EFalse);
	SetRectL(aRect);
	TRAPD(pics, LoadPicturesL());
	(void)pics;                          // (no PsiBoost.mbm: words only)
	ActivateL();
	}

CSpView::~CSpView()
	{
	CGraphicsDevice* d = iCoeEnv->ScreenDevice();
	if (iBig) d->ReleaseFont(iBig);
	if (iName) d->ReleaseFont(iName);
	if (iNow) d->ReleaseFont(iNow);
	if (iText) d->ReleaseFont(iText);
	if (iLastFont) d->ReleaseFont(iLastFont);
	if (iSmall) d->ReleaseFont(iSmall);
	for (TInt i = 0; i < KTileCount; i++)
		{
		delete iPic[i];
		delete iMask[i];
		}
	}

// the choices' pictures, from PsiBoost.mbm (speed/mktbar.py)
void CSpView::LoadPicturesL()
	{
	const TInt ids[KTileCount] = { EMbmPageAlways, EMbmPageFree, EMbmPageTest };
	TFileName mbm = iEikonEnv->EikAppUi()->Application()->BitmapStoreName();
	for (TInt i = 0; i < KTileCount; i++)
		{
		iPic[i] = iEikonEnv->CreateBitmapL(mbm, ids[i]);
		iMask[i] = iEikonEnv->CreateBitmapL(mbm, ids[i] + 1);
		}
	}

static void DrawClipped(CWindowGc& aGc, const CFont* aFont, const TDesC& aText, TPoint aAt, TInt aWidth)
	{
	if (aWidth <= 0)
		return;
	if (aFont->TextWidthInPixels(aText) <= aWidth)
		{
		aGc.DrawText(aText, aAt);
		return;
		}
	_LIT(KDots, "...");
	TInt n = aFont->TextCount(aText, aWidth - aFont->TextWidthInPixels(KDots));
	TBuf<130> t(aText.Left(n < 120 ? n : 120));
	t.Append(KDots);
	aGc.DrawText(t, aAt);
	}

// ----- the page --------------------------------------------------------------------
// The status at the top (three lines), three choices side by side under it.
// The pictures are 64x48.
const TInt KPageMargin = 8;        // round the page
const TInt KTileGap = 8;           // between the choices
const TInt KTileShadow = 2;        // a raised choice's shadow, right and bottom
const TInt KPicW = 64, KPicH = 48;
const TInt KTileLines = 3;         // a choice's state takes at most this many lines
const TInt KNameLines = 2;         // ...and its name

TInt CSpView::TilesTop() const
	{
	return Rect().iTl.iY + KPageMargin + iBig->HeightInPixels() + 3 + iNow->HeightInPixels() + 2
		+ iLastFont->HeightInPixels() + 6;
	}

TRect CSpView::TileRect(TInt aTile) const
	{
	TRect r = Rect();
	TInt all = r.Width() - 2 * KPageMargin - (KTileCount - 1) * KTileGap;
	TInt w = all / KTileCount;
	TInt x = r.iTl.iX + KPageMargin + aTile * (w + KTileGap);
	return TRect(x, TilesTop(), x + w, r.iBr.iY - KPageMargin);
	}

TInt CSpView::TileAt(const TPoint& aPoint) const
	{
	for (TInt i = 0; i < KTileCount; i++)
		if (TileRect(i).Contains(aPoint))
			return i;
	return -1;
	}

// a choice's state in at most aMax lines of aWidth, as a person would
// break it: one line if it fits; else after its " - "; else two lines as
// even as they can be; else as many words as fit on each line. The last
// line takes the rest (clipped with "..." when drawn). (PsiCast's.)
static TInt WrapText(const CFont* aFont, const TDesC& aText, TInt aWidth, TPtrC* aLines, TInt aMax)
	{
	if (aMax <= 0)
		return 0;
	TInt len = aText.Length();
	if (aMax == 1 || aFont->TextWidthInPixels(aText) <= aWidth)
		{
		aLines[0].Set(aText);
		return 1;
		}
	_LIT(KDash, " - ");
	TInt dash = aText.Find(KDash);
	if (dash > 0 && aFont->TextWidthInPixels(aText.Left(dash + 2)) <= aWidth)
		{
		aLines[0].Set(aText.Left(dash + 2));
		return 1 + WrapText(aFont, aText.Mid(dash + 3), aWidth, aLines + 1, aMax - 1);
		}
	TInt best = -1, bestW = KMaxTInt;
	for (TInt i = 1; i < len - 1; i++)
		{
		if (aText[i] != ' ')
			continue;
		TInt a = aFont->TextWidthInPixels(aText.Left(i)), b = aFont->TextWidthInPixels(aText.Mid(i + 1));
		TInt wider = a > b ? a : b;
		if (wider <= aWidth && wider < bestW)
			{
			best = i;
			bestW = wider;
			}
		}
	if (best > 0)
		{
		aLines[0].Set(aText.Left(best));
		aLines[1].Set(aText.Mid(best + 1));
		return 2;
		}
	TInt end = -1;
	for (TInt i = 1; i < len; i++)
		if (aText[i] == ' ' && aFont->TextWidthInPixels(aText.Left(i)) <= aWidth)
			end = i;
	if (end < 0)
		{
		end = aText.Locate(' ');
		if (end < 0)
			end = len;
		}
	aLines[0].Set(aText.Left(end));
	if (end >= len)
		return 1;
	return 1 + WrapText(aFont, aText.Mid(end + 1), aWidth, aLines + 1, aMax - 1);
	}

_LIT(KNameAlways, "230,400 bps always");
_LIT(KNameFree, "Free the serial port");
_LIT(KNameTest, "Test the speed");

static const TDesC& TileName(TInt aTile)
	{
	if (aTile == ESpTileAlways)
		return KNameAlways;
	return aTile == ESpTileFree ? (const TDesC&)KNameFree : (const TDesC&)KNameTest;
	}

// One choice: a picture, its name in bold, how it is now under it. At rest
// it is raised (white, a black edge, a dark grey shadow); while on - or
// while the pen is down on it - it is pressed in (pale grey, as a latched
// button). The one the arrow keys are on has a thick black edge.
void CSpView::DrawTile(CWindowGc& aGc, TInt aTile, TInt aLines) const
	{
	TBuf<100> state;
	TBool on = EFalse, dimmed = EFalse;
	iUi.TileState(aTile, state, on, dimmed);
	TBool in = on || iPressed == aTile;
	TRect t = TileRect(aTile);
	TRect face(t.iTl, TPoint(t.iBr.iX - KTileShadow, t.iBr.iY - KTileShadow));
	if (in)
		face.Move(1, 1);
	else
		{
		aGc.SetPenStyle(CGraphicsContext::ENullPen);
		aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
		aGc.SetBrushColor(KRgbDarkGray);
		aGc.DrawRect(TRect(face.iBr.iX, t.iTl.iY + KTileShadow, t.iBr.iX, t.iBr.iY));
		aGc.DrawRect(TRect(t.iTl.iX + KTileShadow, face.iBr.iY, face.iBr.iX, t.iBr.iY));
		}
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(KRgbBlack);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(in ? KRgbGray : KRgbWhite);
	aGc.DrawRect(face);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	TInt edge = 1;
	if (iFocus == aTile)
		{
		TRect f(face);
		for (; edge < 3; edge++)
			{
			f.Shrink(1, 1);
			aGc.DrawRect(f);
			}
		}
	if (in)
		{
		aGc.SetPenColor(KRgbDarkGray);
		aGc.DrawLine(TPoint(face.iTl.iX + edge, face.iTl.iY + edge), TPoint(face.iBr.iX - edge, face.iTl.iY + edge));
		aGc.DrawLine(TPoint(face.iTl.iX + edge, face.iTl.iY + edge), TPoint(face.iTl.iX + edge, face.iBr.iY - edge));
		}
	TInt textW = face.Width() - 2 * (3 + 6);
	TPtrC names[KNameLines];
	TInt nn = WrapText(iName, TileName(aTile), textW, names, KNameLines);
	TPtrC lines[KTileLines];
	TInt n = WrapText(iText, state, textW, lines, aLines);
	TBool pic = iPic[aTile] && iMask[aTile];
	TInt picH = pic ? KPicH + 4 : 0;
	TInt titleH = iName->HeightInPixels();
	TInt lineH = iText->HeightInPixels() + 1;
	TInt block = picH + KNameLines * titleH + 3 + aLines * lineH;
	TInt y = face.iTl.iY + (face.Height() - block) / 2;
	if (y < face.iTl.iY + edge + 2)
		y = face.iTl.iY + edge + 2;
	TInt cx = (face.iTl.iX + face.iBr.iX) / 2;
	if (pic)
		{
		aGc.BitBltMasked(TPoint(cx - KPicW / 2, y), iPic[aTile], TRect(TSize(KPicW, KPicH)), iMask[aTile], ETrue);
		y += picH;
		}
	// (a one-line name sits low, next to its state)
	y += (KNameLines - nn) * titleH;
	aGc.SetPenColor(dimmed ? KRgbDarkGray : KRgbBlack);
	aGc.UseFont(iName);
	for (TInt i = 0; i < nn; i++)
		{
		TInt w = iName->TextWidthInPixels(names[i]);
		DrawClipped(aGc, iName, names[i], TPoint(w < textW ? cx - w / 2 : cx - textW / 2, y + iName->AscentInPixels()), textW);
		y += titleH;
		}
	y += 3;
	aGc.UseFont(iText);
	for (TInt i = 0; i < n; i++)
		{
		TInt w = iText->TextWidthInPixels(lines[i]);
		DrawClipped(aGc, iText, lines[i], TPoint(w < textW ? cx - w / 2 : cx - textW / 2, y + iText->AscentInPixels()), textW);
		y += lineH;
		}
	}

// the lines the tallest of the choices' states takes (1 at least): every
// choice keeps that room, so their names line up
TInt CSpView::StateLines() const
	{
	TInt lines = 1;
	for (TInt i = 0; i < KTileCount; i++)
		{
		TBuf<100> state;
		TBool on, dimmed;
		iUi.TileState(i, state, on, dimmed);
		TPtrC l[KTileLines];
		TRect t = TileRect(i);
		TInt n = WrapText(iText, state, t.Width() - KTileShadow - 2 * (3 + 6), l, KTileLines);
		if (n > lines)
			lines = n;
		}
	return lines;
	}

// The status: "230400 always: on" in the big type, the hook, and the last
// thing that happened (black, a size smaller: it must be easy to read)
void CSpView::DrawStatus(CWindowGc& aGc) const
	{
	TRect r = Rect();
	TInt x0 = r.iTl.iX + KPageMargin, w = r.Width() - 2 * KPageMargin;
	TBuf<40> always;
	TBuf<100> hook, last;
	iUi.StatusTexts(always, hook, last);
	TInt y = r.iTl.iY + KPageMargin;
	aGc.SetPenColor(KRgbBlack);
	aGc.UseFont(iBig);
	DrawClipped(aGc, iBig, always, TPoint(x0, y + iBig->AscentInPixels()), w);
	y += iBig->HeightInPixels() + 3;
	aGc.UseFont(iNow);
	DrawClipped(aGc, iNow, hook, TPoint(x0, y + iNow->AscentInPixels()), w);
	y += iNow->HeightInPixels() + 2;
	aGc.UseFont(iLastFont);
	DrawClipped(aGc, iLastFont, last, TPoint(x0, y + iLastFont->AscentInPixels()), w);
	}

// The results page: the verdict in bold, then a table - 115200, 230400,
// Gain across; the download, the echo, the round trip, stalls and bad
// bytes down
void CSpView::DrawResults(CWindowGc& aGc) const
	{
	TRect r = Rect();
	TInt x0 = r.iTl.iX + KPageMargin + 8, x1 = r.iBr.iX - KPageMargin - 8;
	TBuf<200> verdict;
	TSpCell cells[KResCells];
	TBool pass = EFalse;
	iUi.ResultTexts(verdict, cells, pass);
	TInt y = r.iTl.iY + KPageMargin + 2;
	aGc.SetPenColor(KRgbBlack);
	aGc.UseFont(iName);
	aGc.DrawText(_L("Test results"), TPoint(x0, y + iName->AscentInPixels()));
	y += iName->HeightInPixels() + 6;
	// (the verdict in the big type when it fits, else the names': a FAIL line is long)
	const CFont* vf = iBig->TextWidthInPixels(verdict) <= x1 - x0 ? iBig : iName;
	aGc.UseFont(vf);
	DrawClipped(aGc, vf, verdict, TPoint(x0, y + vf->AscentInPixels()), x1 - x0);
	y += iBig->HeightInPixels() + 8;
	aGc.SetPenColor(KRgbDarkGray);
	aGc.DrawLine(TPoint(x0, y), TPoint(x1, y));
	y += 6;
	// the columns: the row's name, then three figures right-aligned
	_LIT(KRow0, "Download");
	_LIT(KRow1, "Echo, both ways");
	_LIT(KRow2, "Round trip");
	_LIT(KRow3, "Stalls");
	_LIT(KRow4, "Bad bytes");
	_LIT(KRow5, "Tested");
	const TDesC* rows[KResRows] = { &KRow0, &KRow1, &KRow2, &KRow3, &KRow4, &KRow5 };
	_LIT(KCol0, "115,200 bps");
	_LIT(KCol1, "230,400 bps");
	_LIT(KCol2, "Gain");
	const TDesC* cols[3] = { &KCol0, &KCol1, &KCol2 };
	// (all black, the rows a size up: grey and the smallest type were too
	// faint to read on a real 5mx)
	const CFont* rowFont = iLastFont;
	TInt nameW = rowFont->TextWidthInPixels(KRow1) + 24;
	TInt colW = (x1 - x0 - nameW) / 3;
	TInt lineH = rowFont->HeightInPixels() + 4;
	aGc.UseFont(iName);
	aGc.SetPenColor(KRgbBlack);
	for (TInt c = 0; c < 3; c++)
		{
		TInt w = iName->TextWidthInPixels(*cols[c]);
		aGc.DrawText(*cols[c], TPoint(x0 + nameW + (c + 1) * colW - w, y + iName->AscentInPixels()));
		}
	y += iName->HeightInPixels() + 3;
	aGc.UseFont(rowFont);
	aGc.SetPenColor(KRgbBlack);
	for (TInt i = 0; i < KResRows; i++)
		{
		aGc.DrawText(*rows[i], TPoint(x0, y + rowFont->AscentInPixels()));
		for (TInt c = 0; c < 3; c++)
			{
			const TDesC& v = cells[i * 3 + c];
			TInt w = rowFont->TextWidthInPixels(v);
			aGc.DrawText(v, TPoint(x0 + nameW + (c + 1) * colW - w, y + rowFont->AscentInPixels()));
			}
		y += lineH;
		}
	aGc.UseFont(iText);
	_LIT(KBack, "Esc, or tap, for the page");
	aGc.DrawText(KBack, TPoint(x1 - iText->TextWidthInPixels(KBack), r.iBr.iY - KPageMargin - iText->DescentInPixels()));
	}

void CSpView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	TRect r = Rect();
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.Clear(r);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	if (iResults)
		DrawResults(gc);
	else
		{
		DrawStatus(gc);
		TInt lines = StateLines();
		for (TInt i = 0; i < KTileCount; i++)
			DrawTile(gc, i, lines);
		}
	gc.DiscardFont();
	}

void CSpView::ShowResults(TBool aOn)
	{
	if (iResults == aOn)
		return;
	iResults = aOn;
	DrawNow();
	}

// One tap on a choice acts, as a button does: pressed in while the pen is
// down on it, acting when the pen comes up on it. On the results page any
// tap goes back to the choices.
void CSpView::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	if (iResults)
		{
		if (aEvent.iType == TPointerEvent::EButton1Down)
			ShowResults(EFalse);
		iPressed = iDown = -1;
		return;
		}
	TInt at = TileAt(aEvent.iPosition);
	switch (aEvent.iType)
		{
	case TPointerEvent::EButton1Down:
		iDown = at;
		if (at >= 0)
			{
			InvalidateTile(iFocus);
			iFocus = at;
			iPressed = at;
			InvalidateTile(at);
			}
		break;
	case TPointerEvent::EDrag:
		if (iDown >= 0)
			{
			TInt p = at == iDown ? iDown : -1;
			if (p != iPressed)
				{
				iPressed = p;
				InvalidateTile(iDown);
				}
			}
		break;
	case TPointerEvent::EButton1Up:
		if (iDown >= 0)
			{
			TInt tile = iDown;
			TBool act = iPressed == tile && at == tile;
			iPressed = iDown = -1;
			InvalidateTile(tile);
			if (act)
				iUi.TileActL(tile);
			}
		break;
	default:
		break;
		}
	}

// the arrow keys move between the choices, Enter acts; on the results page
// Esc or Enter goes back; everything else is EIKON's (the shortcuts)
TKeyResponse CSpView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey || (aKeyEvent.iModifiers & EModifierCtrl))
		return EKeyWasNotConsumed;
	if (iResults)
		{
		if (aKeyEvent.iCode == EKeyEscape || aKeyEvent.iCode == EKeyEnter || aKeyEvent.iCode == EKeySpace)
			{
			ShowResults(EFalse);
			return EKeyWasConsumed;
			}
		return EKeyWasNotConsumed;
		}
	switch (aKeyEvent.iCode)
		{
	case EKeyLeftArrow:
		SetFocusTile(iFocus - 1);
		break;
	case EKeyRightArrow:
		SetFocusTile(iFocus + 1);
		break;
	case EKeyHome:
		SetFocusTile(0);
		break;
	case EKeyEnd:
		SetFocusTile(KTileCount - 1);
		break;
	case EKeyEnter:
		iUi.TileActL(iFocus);
		break;
	default:
		return EKeyWasNotConsumed;
		}
	return EKeyWasConsumed;
	}

void CSpView::SetFocusTile(TInt aTile)
	{
	if (aTile < 0 || aTile >= KTileCount || aTile == iFocus)
		return;
	InvalidateTile(iFocus);
	iFocus = aTile;
	InvalidateTile(aTile);
	}

void CSpView::InvalidateTile(TInt aTile)
	{
	if (aTile >= 0 && aTile < KTileCount)
		Window().Invalidate(TileRect(aTile));
	}

// ===========================================================================
// Dialogs
// ===========================================================================

// "host" or "host:port" (your own server): the host, and the base port
// (echo at base+7, chargen at base+19; server/stdsvc.py's 7100 if none is
// given, 0 for a server's standard ports 7 and 19). EFalse if the port is
// not a number from 0 to 65000.
static TBool SplitServer(const TDesC8& aText, TDes8& aHost, TInt& aBase)
	{
	aBase = 7100;
	TInt colon = aText.LocateReverse(':');
	TPtrC8 host(aText);
	if (colon >= 0)
		host.Set(aText.Left(colon));
	aHost.Copy(host.Left(Min(host.Length(), aHost.MaxLength())));
	if (colon < 0)
		return ETrue;
	TPtrC8 port(aText.Mid(colon + 1));
	if (!port.Length() || port.Length() > 5)
		return EFalse;
	TInt v = 0;
	for (TInt i = 0; i < port.Length(); i++)
		{
		if (port[i] < '0' || port[i] > '9')
			return EFalse;
		v = v * 10 + (port[i] - '0');
		}
	if (v > 65000)
		return EFalse;
	aBase = v;
	return ETrue;
	}

// the speeds' choice list: 115200 only, 230400 only, both (the modem told
// each speed in turn)
enum { ESpSpeed115, ESpSpeed230, ESpSpeedBoth };

void CSpTestDialog::PreLayoutDynInitL()
	{
	// (one speed: the one the line is at now; both, if that was last time's
	// and the other end is a modem)
	SetChoiceListCurrentItem(ESpDlgTestSpeed, iS.iTestBoth == 2 && !iNoModem ? ESpSpeedBoth : iHookIn ? ESpSpeed230 : ESpSpeed115);
	// (with a computer the modem route is no use: Psion Internet)
	SetChoiceListCurrentItem(ESpDlgTestRoute, iS.iTestRoute == 1 || iNoModem ? 1 : 0);
	SetChoiceListCurrentItem(ESpDlgTestServer, iS.iTestServer == ESpServerOwn ? 1 : 0);
	SetChoiceListCurrentItem(ESpDlgTestLength, iS.iTestLen >= 0 && iS.iTestLen <= 2 ? iS.iTestLen : 0);
	// (PsiBoost.ini keeps your own server as "host:base" in its string, as
	// before 0.24: the two lines split it)
	TBuf8<60> host8;
	TInt base = 7100;
	SplitServer(iS.iTestHost, host8, base);
	TBuf<60> h;
	h.Copy(host8);
	SetEdwinTextL(ESpDlgTestHost, &h);
	SetNumberEditorValue(ESpDlgTestPort, base);
	iBase = base;
	DimHost();
	}

// the ports your own server needs, from the base port (as stdsvc.py --base)
static void PortsText(TDes& aText, TInt aBase)
	{
	if (aBase == 0)
		aText = _L("Echo 7 and chargen 19: a server's standard ports");
	else
		aText.Format(_L("Echo %d and chargen %d, as server/stdsvc.py with base port %d"), aBase + 7, aBase + 19, aBase);
	}

// the server's address only matters for your own server; the note under
// the speed says what that choice needs
void CSpTestDialog::DimHost()
	{
	TBool own = ChoiceListCurrentItem(ESpDlgTestServer) == 1;
	SetLineDimmedNow(ESpDlgTestHost, !own);
	SetLineDimmedNow(ESpDlgTestPort, !own);
	{
	TBuf<80> ports;
	if (own)
		{
		PortsText(ports, iBase);
		}
	else
		ports = _L("Public servers: a 1 MB file over the web, and an echo");
	CEikLabel* p = (CEikLabel*)Control(ESpDlgTestPorts);
	if (p)
		{
		p->SetTextL(ports);
		p->DrawNow();
		}
	}
	TInt speed = ChoiceListCurrentItem(ESpDlgTestSpeed);
	TPtrC note(speed == ESpSpeed115 ? _L("As the line is now: 230,400 bps always must be off")
	         : speed == ESpSpeed230 ? _L("As the line is now: 230,400 bps always must be on")
	         : iNoModem ? _L("Not available - needs a modem that changes speed with AT commands")
	         : _L("Needs a modem that changes speed with AT commands"));
	CEikLabel* l = (CEikLabel*)Control(ESpDlgTestNote);
	if (l)
		{
		l->SetTextL(note);
		l->DrawNow();
		}
	}

void CSpTestDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == ESpDlgTestServer || aControlId == ESpDlgTestSpeed)
		DimHost();
	}

// (the ports' line follows the base port once its line is left)
void CSpTestDialog::PrepareForFocusTransitionL()
	{
	CEikDialog::PrepareForFocusTransitionL();
	TInt base = NumberEditorValue(ESpDlgTestPort);
	if (base != iBase)
		{
		iBase = base;
		DimHost();
		}
	}

TBool CSpTestDialog::OkToExitL(TInt /*aButtonId*/)
	{
	TInt speed = ChoiceListCurrentItem(ESpDlgTestSpeed);
	TPtrC why(KNullDesC);
	if (speed == ESpSpeedBoth && iNoModem)
		why.Set(_L("Both speeds need a modem - the other end is a computer"));
	else if (speed == ESpSpeed230 && !iHookIn)
		why.Set(_L("230,400 bps always is off - turn it on first"));
	else if (speed == ESpSpeed115 && iHookIn)
		why.Set(_L("230,400 bps always is on - turn it off first"));
	if (why.Length())
		{
		TryChangeFocusToL(ESpDlgTestSpeed);
		iEikonEnv->InfoMsg(why);
		return EFalse;
		}
	if (iNoModem && ChoiceListCurrentItem(ESpDlgTestRoute) != 1)
		{
		TryChangeFocusToL(ESpDlgTestRoute);
		iEikonEnv->InfoMsg(_L("The modem route needs a modem - choose Psion Internet"));
		return EFalse;
		}
	TBuf<60> h;
	GetEdwinText(h, ESpDlgTestHost);
	h.Trim();
	TInt server = ChoiceListCurrentItem(ESpDlgTestServer) == 1 ? ESpServerOwn : ESpServerPublic;
	TInt base = NumberEditorValue(ESpDlgTestPort);
	// (a ":port" typed after the name, as before 0.24, is taken as the base port)
	{
	TBuf8<60> h8, host;
	h8.Copy(h);
	TInt typed = base;
	if (h.Locate(':') >= 0)
		{
		if (!SplitServer(h8, host, typed))
			{
			TryChangeFocusToL(ESpDlgTestHost);
			iEikonEnv->InfoMsg(_L("The port after the ':' must be a number up to 65000"));
			return EFalse;
			}
		h.Copy(host);
		base = typed;
		}
	}
	if (server == ESpServerOwn && !h.Length())
		{
		TryChangeFocusToL(ESpDlgTestHost);
		iEikonEnv->InfoMsg(_L("No server address entered"));
		return EFalse;
		}
	iS.iTestHost.Copy(h);
	if (h.Length())
		iS.iTestHost.AppendFormat(_L8(":%d"), base);
	iS.iTestRoute = ChoiceListCurrentItem(ESpDlgTestRoute);
	iS.iTestServer = server;
	iS.iTestLen = ChoiceListCurrentItem(ESpDlgTestLength);
	iOnly = speed == ESpSpeed115 ? 115200 : speed == ESpSpeed230 ? 230400 : 0;
	iS.iTestBoth = iOnly == 0 ? 2 : 0;
	return ETrue;
	}

void CSpEndDialog::PreLayoutDynInitL()
	{
	SetChoiceListCurrentItem(ESpDlgOtherEnd, iEnd ? 1 : 0);
	if (!iFirst)
		{
		CEikLabel* l = (CEikLabel*)Control(ESpDlgEndLater);
		if (l)
			l->SetTextL(_L("230,400 bps always must be off to change it"));
		}
	ShowNextL();
	}

// what happens next, for the choice the list is on
void CSpEndDialog::ShowNextL()
	{
	TBool computer = ChoiceListCurrentItem(ESpDlgOtherEnd) == 1;
	CEikLabel* l = (CEikLabel*)Control(ESpDlgEndNext);
	if (l)
		{
		l->SetTextL(computer ? _L("Next: set the computer's port to 230,400 bps") : _L("Next: turn on 230,400 bps always"));
		l->DrawNow();
		}
	l = (CEikLabel*)Control(ESpDlgEndHow);
	if (l)
		{
		l->SetTextL(computer ? _L("when 230,400 bps always is on: PsiBoost sends the computer nothing")
		                     : _L("PsiBoost then tells the modem 230,400 bps, and the modem saves it"));
		l->DrawNow();
		}
	}

void CSpEndDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId == ESpDlgOtherEnd)
		ShowNextL();
	}

// (the other end changes only while 230400 always is off: the modem has to
// be told, or has been). The welcome's page has no Cancel, and Esc stays.
TBool CSpEndDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId == EEikBidCancel)
		{
		if (!iFirst)
			return ETrue;
		iEikonEnv->InfoMsg(_L("Choose the other end, then Continue"));
		return EFalse;
		}
	TInt end = ChoiceListCurrentItem(ESpDlgOtherEnd) == 1 ? 1 : 0;
	if (end != iEnd && iAlways)
		{
		iEikonEnv->InfoMsg(_L("Turn 230,400 bps always off first"));
		return EFalse;
		}
	iEnd = end;
	return ETrue;
	}

TBool CSpWelcomeDialog::OkToExitL(TInt aButtonId)
	{
	if (aButtonId != EEikBidCancel)
		return ETrue;
	iEikonEnv->InfoMsg(_L("Continue to set up PsiBoost"));
	return EFalse;
	}

void CSpAboutDialog::PreLayoutDynInitL()
	{
	TBuf<32> title(_L("PsiBoost "));
	title.Append(KVersion);
	((CEikLabel*)Control(ESpDlgAbout1))->SetFont(iEikonEnv->TitleFont());
	SetLabelL(ESpDlgAbout1, title);
	SetLabelL(ESpDlgAboutStatus, iStatus);
	}

struct TSpHelpTopic { const char* iTitle; const char* iText; };

static const TSpHelpTopic KHelpTopics[] =
	{
	{ "What it does",
	  "PsiBoost runs the Psion's serial port at 230,400 bps instead of 115,200 bps, twice as fast, for "
	  "every program: PsiTerm, PsiMail, PsiWeb, PsiCast and Psion Internet. The Psion's own "
	  "serial driver stops at 115,200 bps, so PsiBoost puts a small hook into it: when a program "
	  "asks the port for 115,200 bps, it gets 230,400 bps from the first byte. Other speeds are left "
	  "alone. The modem on the end of the cable is told 230,400 bps and keeps it. The Remote link "
	  "to a PC (PsiWin) at 115,200 bps would get 230,400 bps too: set it to 57,600 bps, or turn 230,400 bps "
	  "always off, before using it." },
	{ "Before you use it",
	  "PsiBoost has only been tested on a Psion Series 5mx with ROM 1.05(260), with the "
	  "PsiConnect Wi-Fi modem. On another Psion or ROM, 230,400 bps always is not available.\n"
	  "\n"
	  "The hook runs inside the Psion's operating system: a mistake in it could reset the "
	  "Psion and lose what is on the Internal disk (C:), so back it up first. It cannot harm "
	  "the Psion itself.\n"
	  "\n"
	  "If it works on another Psion, or with another modem, say so on PsiBoost's GitHub page "
	  "(github.com/danieledge/psiboost)." },
	{ "The first time it opens",
	  "The first time PsiBoost opens, it welcomes you and says what it has been tested on. "
	  "Then it asks what is on the other end of the Psion's serial cable:\n"
	  "\n"
	  "A Wi-Fi or dial-up modem: anything that answers AT commands, such as the PsiConnect "
	  "Wi-Fi modem, a dial-up modem or a phone. PsiBoost tells it the speed.\n"
	  "\n"
	  "A computer or other device: anything that does not answer AT, such as a computer on a "
	  "serial cable or a USB serial adapter, running pppd or similar. PsiBoost sends it "
	  "nothing: set its port to 230,400 bps yourself when 230,400 bps always is on.\n"
	  "\n"
	  "Both pages must be answered with Continue: Esc does not get round them. If PsiBoost is "
	  "closed before then, they come again next time. Speed > The other end changes the "
	  "choice later, while 230,400 bps always is off." },
	{ "230,400 bps always",
	  "Tap 230,400 bps always (or press Enter on it) to turn it on, and again to turn it off. The "
	  "serial port must be free: if a program is using it, close it first, or use Free the "
	  "serial port.\n"
	  "\n"
	  "Turning it on finds the modem at 115,200 bps, tells it 230,400 bps, puts the hook in, checks "
	  "that the modem answers at 230,400 bps, and saves the speed on the modem (AT&W). If any "
	  "step fails, the hook comes out and the modem goes back to 115,200 bps, and PsiBoost says "
	  "why.\n"
	  "\n"
	  "Turning it off does the same the other way: the modem is told 115,200 bps and saves it, "
	  "and the hook comes out. If the modem does not answer, 230,400 bps always is off on the "
	  "Psion anyway: set the modem back to 115,200 bps yourself (see its manual or web page).\n"
	  "\n"
	  "If the other end is a modem but none answers, PsiBoost asks whether to turn it on "
	  "anyway, for a cable to a computer: see With a cable to a computer." },
	{ "With a cable to a computer (no modem)",
	  "With a serial cable straight to a computer (a null-modem cable), or a device whose "
	  "speed you set yourself, choose A computer or other device when PsiBoost first opens, "
	  "or in Speed > The other end (with 230,400 bps always off). If you chose a modem and "
	  "none answers, PsiBoost asks whether to turn 230,400 bps always on anyway, and on Yes "
	  "remembers there is no modem. Then it only puts the hook in or takes it out: turning "
	  "it on and off, start-up and the page send no AT commands.\n"
	  "\n"
	  "Set the computer's port to 230,400 bps yourself, for example on Linux "
	  "pppd /dev/ttyUSB0 230400 ... or stty -F /dev/ttyS0 230400. Most USB serial adapters "
	  "(FTDI, PL2303, CP210x) do 230,400 bps. A PC's own serial port often stops at "
	  "115,200 bps.\n"
	  "\n"
	  "On the Psion, a program must still ask for 115,200 bps: the hook turns that into "
	  "230,400 bps. For Psion Internet, keep the setup you already use with the cable at "
	  "115,200 bps: its modem settings still send their initialisation string and dial, "
	  "so the computer's end must cope with them as before. PsiBoost changes only the speed.\n"
	  "\n"
	  "Test the speed works at one speed at a time, on Psion Internet: 115,200 bps with "
	  "230,400 bps always off, then 230,400 bps with it on, the computer set to match each "
	  "time. Both speeds at once needs a modem." },
	{ "At start-up",
	  "The hook is in the Psion's memory, so restarting the Psion takes it out. PsiBoost "
	  "installs a small file on the CF card, PsiBoost.rdl in D:\\System\\Recogs, which puts "
	  "the hook back as the Psion starts, when 230,400 bps always is on. It checks everything "
	  "first and does nothing if anything is not as expected.\n"
	  "\n"
	  "To start the Psion without the hook: hold down Ctrl while it starts (from when the "
	  "screen comes on until the System screen appears), or create the file "
	  "D:\\System\\Data\\PsiBoost.off. If the Psion will not start properly, take the CF "
	  "card out and restart it: without the card nothing of PsiBoost's runs at start-up.\n"
	  "\n"
	  "If the hook is off when PsiBoost opens but 230,400 bps always is on, PsiBoost puts it back "
	  "and says so. Tools > Restart the Psion restarts it, as the reset button does: the "
	  "files on the Internal disk stay." },
	{ "The page",
	  "The first line says whether 230,400 bps always is on. The second says what the hook is "
	  "doing now, and whether the modem is at the same speed (PsiBoost asks it when it "
	  "opens, and Speed > Check the modem's speed asks it again). With a computer on the "
	  "other end (Speed > The other end) it says what speed the computer must be at. The third is the last "
	  "thing that happened.\n"
	  "\n"
	  "Free the serial port stops Psion Internet if it is connected and waits up to 20 s for "
	  "the port. Test the speed runs a test (see below) and shows its last result." },
	{ "The modem",
	  "PsiBoost tells the modem 230,400 bps with AT$SB=230400, ATB230400 or AT+IPR=230400, "
	  "whichever it takes, and saves it with AT&W; a modem that cannot save its speed is not "
	  "used. A modem that takes AT$SB changes speed after its OK, and goes back by itself in "
	  "15 s if nothing is heard at the new speed." },
	{ "The log",
	  "Each step is written to PsiBoost.log in PsiBoost's folder, with what the modem said, "
	  "and the start-up file adds a line there each time the Psion starts. When it is full it "
	  "becomes PsiBoost.old and a new one is started.\n"
	  "\n"
	  "Tools > Show log shows the end of it. Tools > Send log to server sends it to your "
	  "PsiProxy: the proxy set in Connection settings (in PsiTerm, PsiMail or PsiWeb). "
	  "Nothing is sent anywhere unless you ask." },
	{ "Testing the speed",
	  "Tools > Test the speed runs a download and an echo on one route against one server, "
	  "every byte checked, stalls and drops counted. Speed says at what:\n"
	  "\n"
	  "115,200 bps or 230,400 bps: that speed only, with the line as it is now. PsiBoost "
	  "changes nothing, so 230,400 bps always must be off for 115,200 bps and on for "
	  "230,400 bps, and the other end must be at the same speed. This works with a modem or "
	  "a computer. Only that column of the results changes; the other keeps its last result, "
	  "and when both have passed the verdict compares them.\n"
	  "\n"
	  "Both: 115,200 bps and then 230,400 bps, back to back, the modem told each speed in "
	  "turn. It needs a modem that changes speed with AT commands. The second half asks the "
	  "port for 115,200 bps and gets 230,400 bps through the hook, as every program does. "
	  "Afterwards the modem and the hook are put back as 230,400 bps always says.\n"
	  "\n"
	  "Public (internet) uses a 1 MB file on a public web server and a public echo.\n"
	  "\n"
	  "Your own server uses an echo and a chargen service on a computer the Psion can reach. "
	  "Choose Your own server, then type the computer's name or address in Server name or "
	  "address, and its base port in Base port. The echo is on the base port + 7 and the "
	  "chargen on the base port + 19: 7100 means 7107 and 7119. A base port of 0 means a "
	  "server's standard ports, 7 and 19. The line under it says which ports, and the page "
	  "shows the server under Test the speed.\n"
	  "\n"
	  "To run the server, copy server/stdsvc.py from PsiBoost's GitHub page to the computer "
	  "and run python3 stdsvc.py there (it needs only Python 3). It listens on all the "
	  "computer's addresses with the base port 7100: echo 7107, discard 7109, chargen 7119. "
	  "python3 stdsvc.py --base N moves them to N+7, N+9 and N+19; the Psion's Base port must "
	  "then be N. Let those ports through the computer's firewall.\n"
	  "\n"
	  "The results come side by side: the download and echo rates, the round trip, stalls "
	  "and bad bytes at each speed, the gain, and when each speed was tested. With a "
	  "computer, use Psion Internet: the modem route needs a modem. Tools > Last test "
	  "results shows them again." },
	};
const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

void CSpHelpDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* topics = new(ELeave) CDesCArrayFlat(KHelpCount);
	CleanupStack::PushL(topics);
	for (TInt i = 0; i < KHelpCount; i++)
		{
		TBuf<40> t;
		t.Copy(TPtrC8((const TUint8*)KHelpTopics[i].iTitle));
		topics->AppendL(t);
		}
	CEikChoiceList* list = (CEikChoiceList*)Control(ESpDlgHelpTopic);
	list->SetArrayL(topics);
	CleanupStack::Pop();
	list->SetCurrentItem(0);
	iTopic = 0;
	ShowTopicL(0);
	}

void CSpHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TSpHelpTopic& topic = KHelpTopics[aTopic];
	TPtrC8 title((const TUint8*)topic.iTitle);
	TPtrC8 text((const TUint8*)topic.iText);
	HBufC* b = HBufC::NewLC(title.Length() + text.Length() + 4);
	TPtr p = b->Des();
	p.Copy(title);
	p.Append(CEditableText::EParagraphDelimiter);
	for (TInt i = 0; i < text.Length(); i++)
		{
		TUint8 c = text[i];
		if (c == '\n')
			p.Append(CEditableText::EParagraphDelimiter);
		else
			p.Append((TText)c);
		}
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ESpDlgHelpText);
	ed->SetTextL(b);
	TCharFormat cf;
	TCharFormatMask cm;
	cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	cm.SetAttrib(EAttFontStrokeWeight);
	ed->RichText()->ApplyCharFormatL(cf, cm, 0, title.Length());
	ed->HandleTextChangedL();
	ed->SetCursorPosL(0, EFalse);
	CleanupStack::PopAndDestroy();
	}

void CSpHelpDialog::PostLayoutDynInitL()
	{
	TryChangeFocusToL(ESpDlgHelpTopic);
	}

void CSpHelpDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != ESpDlgHelpTopic)
		return;
	TInt t = ((CEikChoiceList*)Control(ESpDlgHelpTopic))->CurrentItem();
	if (t != iTopic && t >= 0 && t < KHelpCount)
		{
		iTopic = t;
		ShowTopicL(t);
		}
	}

// Tools > Show log: the end of PsiBoost.log, read only, at its last line.
// The text never takes the focus (so no cursor is drawn): the arrow keys
// scroll it from the dialog.
void CSpLogDialog::PreLayoutDynInitL()
	{
	SetLineNonFocusing(ESpDlgLogText);
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ESpDlgLogText);
	HBufC* t = iText;
	iText = NULL;                        // (SetTextL copies it; it is ours to delete)
	CleanupStack::PushL(t);
	ed->SetTextL(t);
	CleanupStack::PopAndDestroy();
	ed->HandleTextChangedL();
	}

void CSpLogDialog::PostLayoutDynInitL()
	{
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ESpDlgLogText);
	ShowEndL();
	}

// the last page of the log in view: the end put at the top, then the view
// taken back a line at a time until the end is at the bottom
void CSpLogDialog::ShowEndL()
	{
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ESpDlgLogText);
	ed->SetCursorPosL(ed->TextLength(), EFalse);
	if (!ed->TextView())
		return;
	for (TInt i = 0; i < KLogViewLines - 1; i++)
		if (!ed->TextView()->ScrollDisplayL(TCursorPosition::EFLineUp))
			break;
	ed->TextView()->SetCursorVisibilityL(TCursor::EFCursorInvisible, TCursor::EFCursorInvisible);
	}

TKeyResponse CSpLogDialog::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(ESpDlgLogText);
	if (aType == EEventKey && ed && ed->TextView())
		{
		TBool ctrl = (aKeyEvent.iModifiers & EModifierCtrl) != 0;
		switch (aKeyEvent.iCode)
			{
		case EKeyUpArrow:
		case EKeyDownArrow:
		case EKeyPageUp:
		case EKeyPageDown:
			{
			TBool up = aKeyEvent.iCode == EKeyUpArrow || aKeyEvent.iCode == EKeyPageUp;
			TBool page = ctrl || aKeyEvent.iCode == EKeyPageUp || aKeyEvent.iCode == EKeyPageDown;
			TCursorPosition::TMovementType m = page ? (up ? TCursorPosition::EFPageUp : TCursorPosition::EFPageDown)
			                                        : (up ? TCursorPosition::EFLineUp : TCursorPosition::EFLineDown);
			ed->TextView()->ScrollDisplayL(m);
			return EKeyWasConsumed;
			}
		case EKeyHome:
			ed->SetCursorPosL(0, EFalse);
			ed->TextView()->SetCursorVisibilityL(TCursor::EFCursorInvisible, TCursor::EFCursorInvisible);
			return EKeyWasConsumed;
		case EKeyEnd:
			ShowEndL();
			return EKeyWasConsumed;
		default:
			break;
			}
		}
	return CEikDialog::OfferKeyEventL(aKeyEvent, aType);
	}

// ===========================================================================
// The app
// ===========================================================================

TFileName CSpAppUi::AppDir() const
	{
	TParsePtrC p(Application()->AppFullName());
	TFileName d(p.DriveAndPath());
	return d;
	}

void CSpAppUi::ConstructL()
	{
	BaseConstructL();
	LoadSettings();
	ReadRoute();
	if (iToolBar)
		{
		TRAPD(pics, ButtonPictureL(ESpCmdFast, EMbmToolAuto);
			ButtonPictureL(ESpCmdFree, EMbmToolFree);
			ButtonPictureL(ESpCmdTest, EMbmToolTest);
			ButtonPictureL(ESpCmdResults, EMbmToolResults));
		(void)pics;                      // (no PsiBoost.mbm: words only)
		CEikButtonBase* b = (CEikButtonBase*)iToolBar->ControlById(ESpCmdFast);
		if (b)
			b->SetBehavior(EEikButtonLatches);
		if (!iSettings.iToolbar)
			iToolBar->MakeVisible(EFalse);
		}
	OpenHook();
	RefreshHook();
	HookAtStart();
	OpenChunkL();
	if (iS)
		{
		iS->iAuto = iSettings.iAlways;
		iS->iNoModem = iSettings.iOtherEnd;
		iS->iAppOpen = 1;
		iS->iAppBeat++;
		iS->iQuit = 0;
		iS->iAppSeq++;
		iS->iModemSpeed = KSpModemNotLooked;
		}
	// (the helper runs while PsiBoost is open: it looks at the modem now)
	TInt r = EnsureHelper();
	if (r != KErrNone && iS)
		HelperMissing(r);
	else if (!NoModem())
		StartCheck();
	iView = new(ELeave) CSpView(*this);
	iView->ConstructL(ViewRect());
	AddToStackL(iView);
	iTimer = CPeriodic::NewL(CActive::EPriorityStandard);
	iTimer->Start(KTickUs, KTickUs, TCallBack(Tick, this));
	ButtonStates();
	// (the first time: the welcome and the other end, once the page is up)
	iWarnPending = iSettings.iWarned < 2;
	}

CSpAppUi::~CSpAppUi()
	{
	delete iTimer;
	if (iBusy != ESpIdle)
		iEikonEnv->BusyMsgCancel();
	if (iView)
		{
		RemoveFromStack(iView);
		delete iView;
		}
	if (iS)
		iS->iAppOpen = 0;
	if (iHelperOpen)
		iHelper.Close();
	if (iChunkOpen)
		iChunk.Close();                  // (the helper keeps its own handle while it runs)
	if (iHookOpen)
		iHook.Close();                   // (the hook stays: it is in the driver, not this handle)
	}

// ----- the hook -------------------------------------------------------------------

// the hook's driver, for the ROM it was made for only
void CSpAppUi::OpenHook()
	{
	iHookFound = EPbNoFactory;
	TMachineInfoV1Buf mi;
	iHookErr = KErrNotSupported;
	if (UserHal::MachineInfo(mi) != KErrNone)
		return;
	const TVersion& v = mi().iRomVersion;
	if (v.iMajor != KPbRomMajor || v.iMinor != KPbRomMinor || v.iBuild != KPbRomBuild
		|| mi().iDisplaySizeInPixels != TSize(640, 240))
		return;
	TInt r = User::LoadLogicalDevice(KPbHookFile);
	if (r == KErrNone || r == KErrAlreadyExists)
		r = iHook.Open();
	if (r == KErrNone && ((TUint)iHook.Version() >> 16) != (TUint)KPbHookMagic)
		{
		iHook.Close();
		r = KErrNotSupported;
		}
	iHookOpen = r == KErrNone;
	iHookErr = r;
	}

// the hook as it is now: ETrue if that changed
TBool CSpAppUi::RefreshHook()
	{
	TInt was = iHookFound;
	if (iHookOpen)
		iHookFound = PbFindFactory(iHook, iFactory);
	return iHookFound != was;
	}

// 230400 always on in PsiBoost.ini, but the hook is off (the Psion has been
// restarted without the recogniser on the CF card): put back, and said. If
// the hook is on, the line the recogniser wrote at start-up is shown.
void CSpAppUi::HookAtStart()
	{
	if (!iHookOpen)
		return;
	if (iSettings.iAlways && iHookFound == EPbStock)
		{
		TInt r = iHook.Install(iFactory);
		RefreshHook();
		if (r == KErrNone && iHookFound == EPbHooked)
			{
			SetLast(_L("230,400 bps was off (a restart?) - PsiBoost has turned it back on"));
			iEikonEnv->InfoMsg(_L("230,400 bps is back on"));
			}
		else
			{
			TBuf<80> m;
			m.Format(_L("230,400 bps was off and could not be turned back on (%d)"), r);
			SetLast(m);
			iEikonEnv->InfoMsg(m);
			}
		}
	else if (iHookFound == EPbHooked)
		LastStartUp(iLast);
	else if (iSettings.iAlways && iHookFound == EPbForeign)
		iEikonEnv->InfoMsg(_L("The serial port is hooked by another program - restart the Psion"));
	}

// the last "Start-up:" line PsiBoost.rdl wrote in PsiBoost.log, as
// "At start-up, hh:mm: ..." (empty if there is none)
void CSpAppUi::LastStartUp(TDes& aLine) const
	{
	aLine.Zero();
	TFileName name = AppDir();
	name.Append(KLogName);
	RFile f;
	if (f.Open(iCoeEnv->FsSession(), name, EFileRead | EFileShareAny) != KErrNone)
		return;
	TInt size = 0;
	f.Size(size);
	TBuf8<2048> d;
	TInt pos = size > d.MaxLength() ? size - d.MaxLength() : 0;
	f.Seek(ESeekStart, pos);
	TInt r = f.Read(d);
	f.Close();
	if (r != KErrNone)
		return;
	_LIT8(KMark, " Start-up: ");
	TInt at = -1;
	for (TInt i = 0; i + KMark().Length() <= d.Length(); i++)
		if (d.Mid(i, KMark().Length()) == KMark)
			at = i;
	if (at < 5)
		return;
	TPtrC8 rest(d.Mid(at + KMark().Length()));
	TInt eol = rest.Locate('\r');
	if (eol >= 0)
		rest.Set(rest.Left(eol));
	TBuf<12> hm;
	hm.Copy(d.Mid(at - 8, 5));           // ("hh:mm:ss Start-up: ...")
	aLine.Format(_L("At start-up, %S: "), &hm);
	TBuf<100> t;
	t.Copy(rest.Left(Min(rest.Length(), aLine.MaxLength() - aLine.Length())));
	aLine.Append(t);
	}

// a line in PsiBoost.log from PsiBoost itself (the helper writes the rest),
// as the helper writes them: the time, the text. The helper keeps the log
// small (PsiBoost.old)
void CSpAppUi::AppLog(const TDesC& aLine) const
	{
	TFileName name = AppDir();
	name.Append(KLogName);
	RFs& fs = iCoeEnv->FsSession();
	RFile f;
	TInt r = f.Open(fs, name, EFileWrite | EFileShareAny);
	if (r == KErrNotFound)
		r = f.Create(fs, name, EFileWrite | EFileShareAny);
	if (r != KErrNone)
		return;
	TInt end = 0;
	f.Seek(ESeekEnd, end);
	TTime t;
	t.HomeTime();
	TDateTime d = t.DateTime();
	TBuf8<260> l;
	l.Format(_L8("%02d:%02d:%02d "), d.Hour(), d.Minute(), d.Second());
	TBuf8<240> a;
	a.Copy(aLine.Left(Min(aLine.Length(), a.MaxLength())));
	l.Append(a);
	l.Append(_L8("\r\n"));
	f.Write(l);
	f.Close();
	}

void CSpAppUi::SetLast(const TDesC& aText)
	{
	iLast = aText.Left(Min(aText.Length(), iLast.MaxLength()));
	}

// The shared chunk: the helper's, if one is running and it is this
// PsiBoost's (the same layout and version); an older one is asked to quit
// first, and a new chunk is made. An old helper that will not quit is kept
// if its layout fits, else PsiBoost says so and runs without one.
void CSpAppUi::OpenChunkL()
	{
	iS = NULL;
	TInt r = iChunk.OpenGlobal(KSpChunk, EFalse);
	if (r == KErrNone)
		{
		iChunkOpen = ETrue;
		TSpShared* old = (TSpShared*)iChunk.Base();
		TBool same = old->iMagic == KSpMagic && old->iLayout == KSpLayout;
		TBool running = old->iMagic == KSpMagic && (same ? old->iState != ESpStopped : ETrue);
		TBool stale = same && old->iVersion != KVersion8;
		if (same && !stale)
			{
			iS = old;
			iHelperStarted = running;
			}
		else if (running)
			{
			// (iQuit is one of the eight fields that never move)
			iEikonEnv->BusyMsgL(_L("Restarting PsiBoost's helper..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(0));
			old->iQuit = 1;
			TBool gone = HelperQuitL();
			iEikonEnv->BusyMsgCancel();
			if (gone)
				{
				iChunk.Close();
				iChunkOpen = EFalse;
				r = KErrNotFound;
				}
			else if (same)
				{
				iS = old;
				iHelperStarted = ETrue;
				iEikonEnv->InfoMsg(_L("An older helper is still running - close the program using the serial port, then open PsiBoost again"));
				}
			else
				{
				iChunk.Close();
				iChunkOpen = EFalse;
				iEikonEnv->InfoMsg(_L("Not available - an older helper is still running: restart the Psion"));
				return;
				}
			}
		else
			{
			iChunk.Close();
			iChunkOpen = EFalse;
			r = KErrNotFound;
			}
		}
	if (r != KErrNone)
		{
		r = iChunk.CreateGlobal(KSpChunk, sizeof(TSpShared), sizeof(TSpShared));
		if (r != KErrNone)
			{
			iEikonEnv->InfoMsg(_L("Not available - an older helper is still running: restart the Psion"));
			return;
			}
		iChunkOpen = ETrue;
		iS = (TSpShared*)iChunk.Base();
		Mem::FillZ(iS, sizeof(TSpShared));
		// (the descriptors in it are made in place: zeros would give them
		// no room, and the first Copy would panic)
		new(&iS->iMsg) TBuf8<96>;
		new(&iS->iVersion) TBuf8<8>;
		new(&iS->iTestHost) TBuf8<64>;
		new(&iS->iTestNow) TBuf8<100>;
		new(&iS->iTestVerdict) TBuf8<200>;
		iS->iMagic = KSpMagic;
		iS->iLayout = KSpLayout;
		iS->iState = ESpStopped;
		iS->iDivider = -1;
		iS->iModemSpeed = KSpModemNotLooked;
		iHelperStarted = EFalse;
		}
	}

// waits for a helper that has been asked to quit: ETrue when none is running
TBool CSpAppUi::HelperQuitL()
	{
	for (TInt waited = 0; waited < KHelperQuitWaitMs; waited += 250)
		{
		TFindProcess find(KHelperMatch);
		TFullName name;
		if (find.Next(name) != KErrNone)
			return ETrue;
		User::After(250000);
		}
	return EFalse;
	}

TRect CSpAppUi::ViewRect() const
	{
	TRect r = ClientRect();
	if (iToolBar && !iToolBar->IsVisible())
		r.iBr.iX = iCoeEnv->ScreenDevice()->SizeInPixels().iWidth;
	return r;
	}

// a toolbar button's picture and mask from PsiBoost.mbm
void CSpAppUi::ButtonPictureL(TInt aId, TInt aIcon)
	{
	CEikCommandButton* b = (CEikCommandButton*)iToolBar->ControlById(aId);
	if (!b)
		return;
	TFileName mbm = Application()->BitmapStoreName();
	CFbsBitmap* bmp = iEikonEnv->CreateBitmapL(mbm, aIcon);
	CleanupStack::PushL(bmp);
	CFbsBitmap* mask = iEikonEnv->CreateBitmapL(mbm, aIcon + 1);
	CleanupStack::PushL(mask);
	b->SetPictureL(bmp, mask);
	CleanupStack::Pop(2);
	if (b->Picture())
		b->Picture()->SetAlignment(EHCenterVCenter);
	if (b->Label())
		b->Label()->SetAlignment(EHLeftVCenter);
	b->LayoutComponentsL();
	}

// Connection settings (PsiLink.ini): the test's route the first time
void CSpAppUi::ReadRoute()
	{
	TPsiLink link;
	link.SetDefaults();
	if (link.Load(iCoeEnv->FsSession()))
		{
		iRoute = link.iNetMode ? 1 : 0;
		iBaudIndex = link.iBaudIndex;
		}
	else
		iRoute = iBaudIndex = -1;
	}

// ----- settings -----------------------------------------------------------------

static TInt Int32At(const TUint8* aP)
	{
	return aP[0] | (aP[1] << 8) | (aP[2] << 16) | (aP[3] << 24);
	}

void CSpAppUi::LoadSettings()
	{
	Mem::FillZ(&iSettings, sizeof(iSettings));
	new(&iSettings.iTestHost) TBuf8<60>;
	new(&iSettings.iVerdict) TBuf8<200>;
	iSettings.iToolbar = 1;
	iSettings.iTestRoute = -1;
	iSettings.iTestServer = ESpServerPublic;
	TFileName ini = AppDir();
	ini.Append(KIniName);
	RFile f;
	if (f.Open(iCoeEnv->FsSession(), ini, EFileRead) != KErrNone)
		return;
	TBuf8<8 + 4 * 32 + 1 + 60 + 1 + 200> d;
	TInt r = f.Read(d);
	f.Close();
	if (r != KErrNone || d.Length() < 8)
		return;
	const TUint8* p = d.Ptr();
	TInt version = Int32At(p);
	TInt count = Int32At(p + 4);
	if (count < 0 || count > 32)
		return;
	// (fields 3 to 21 mean the same in every version since 0.18, the toolbar
	// and the test's since 0.10; field 0 is 230400 always from version 4: an
	// older PsiBoost's choice is not carried over, as the modem has to be told)
	TInt* field = &iSettings.iAlways;
	TInt take = version >= 3 ? KSpFields : version == 2 ? 22 : 5;
	for (TInt i = 0; i < count && i < take && 8 + 4 * i + 4 <= d.Length(); i++)
		if (i >= 3 || version >= KSpIniVersion)
			field[i] = Int32At(p + 8 + 4 * i);
	// then the strings: your own server, and (version 2 on) the verdict
	TInt sp = 8 + 4 * count;
	if (sp < d.Length())
		{
		TInt hl = p[sp];
		if (sp + 1 + hl <= d.Length() && hl <= iSettings.iTestHost.MaxLength())
			iSettings.iTestHost.Copy(d.Mid(sp + 1, hl));
		sp += 1 + hl;
		if (version >= 2 && sp < d.Length())
			{
			TInt vl = p[sp];
			if (sp + 1 + vl <= d.Length() && vl <= iSettings.iVerdict.MaxLength())
				iSettings.iVerdict.Copy(d.Mid(sp + 1, vl));
			}
		}
	iSettings.iAlways = iSettings.iAlways == 1 ? 1 : 0;
	iSettings.iWarned = iSettings.iWarned == 1 || iSettings.iWarned == 2 ? iSettings.iWarned : 0;
	iSettings.iOtherEnd = iSettings.iOtherEnd == 1 ? 1 : 0;
	iSettings.iTestBoth = iSettings.iTestBoth == 2 ? 2 : 0;
	// (before 0.24 a test always ran both halves, and field 9 was 1 or 0:
	// a pass was a pass in both columns. Fields 22 and 23 were spare: 0)
	if (version < KSpIniVersion || count < KSpFields)
		iSettings.iResAt[0] = iSettings.iResAt[1] = 0;
	if (iSettings.iResPass == 1)
		iSettings.iResPass = 7;
	for (TInt c = 0; c < 2; c++)
		if (iSettings.iResAt[c] < 0)
			iSettings.iResAt[c] = 0;
	if (iSettings.iTestServer != ESpServerOwn)
		iSettings.iTestServer = ESpServerPublic;
	}

void CSpAppUi::SaveSettings()
	{
	TFileName ini = AppDir();
	ini.Append(KIniName);
	TBuf8<8 + 4 * KSpFields + 1 + 60 + 1 + 200> d;
	const TInt head[2] = { KSpIniVersion, KSpFields };
	const TInt* field = &iSettings.iAlways;
	for (TInt i = 0; i < 2 + KSpFields; i++)
		{
		TInt v = i < 2 ? head[i] : field[i - 2];
		d.Append((TUint8)v);
		d.Append((TUint8)(v >> 8));
		d.Append((TUint8)(v >> 16));
		d.Append((TUint8)(v >> 24));
		}
	d.Append((TUint8)iSettings.iTestHost.Length());
	d.Append(iSettings.iTestHost);
	d.Append((TUint8)iSettings.iVerdict.Length());
	d.Append(iSettings.iVerdict);
	RFile f;
	if (f.Replace(iCoeEnv->FsSession(), ini, EFileWrite) == KErrNone)
		{
		f.Write(d);
		f.Close();
		}
	}

// ----- the helper ----------------------------------------------------------------

TInt CSpAppUi::EnsureHelper()
	{
	if (!iS)
		return KErrNotReady;
	if (iS->iState != ESpStopped && iHelperStarted)
		{
		if (!iHelperOpen)
			{
			// (one running from before PsiBoost opened: found by name, to see it end)
			TFindProcess find(KHelperMatch);
			TFullName name;
			if (find.Next(name) == KErrNone && iHelper.Open(find) == KErrNone)
				iHelperOpen = ETrue;
			}
		// (0.24) and really there: the shared memory can say running after
		// the helper was ended from outside, and a request would then wait
		// for nobody, with nothing in the log
		if (iHelperOpen && iHelper.ExitType() == EExitPending)
			return KErrNone;
		AppLog(_L("PsiBoost's helper was not running: starting it again"));
		iHelperStarted = EFalse;
		}
	if (iHelperOpen)
		{
		iHelper.Close();
		iHelperOpen = EFalse;
		}
	TFileName exe = AppDir();
	exe.Append(KSpHelperExe);
	TInt r = iHelper.Create(exe, KNullDesC);
	if (r != KErrNone)
		{
		TBuf<120> m;
		m.Format(_L("PsiBoost's helper did not start (%d): "), r);
		m.Append(exe.Right(Min(exe.Length(), m.MaxLength() - m.Length())));
		AppLog(m);
		return r;
		}
	iHelperOpen = ETrue;
	iS->iState = ESpStarting;
	iS->iDriver = KErrNone;
	iS->iQuit = 0;
	iHelper.Resume();
	iHelperStarted = ETrue;
	return KErrNone;
	}

// the helper's process has ended without saying so (a panic): the page must
// not go on showing a helper that is not there
void CSpAppUi::HelperDied()
	{
	TExitType t = iHelper.ExitType();
	TBuf<96> m(_L("PsiBoost's helper stopped"));
	if (t == EExitPanic)
		{
		TExitCategoryName cat = iHelper.ExitCategory();
		m.AppendFormat(_L(" - %S %d"), &cat, iHelper.ExitReason());
		}
	else
		m.AppendFormat(_L(" (%d)"), iHelper.ExitReason());
	iHelper.Close();
	iHelperOpen = EFalse;
	iHelperStarted = EFalse;
	if (iS)
		{
		iS->iState = ESpStopped;
		iS->iUartOn = 0;
		if (iS->iTestState == ESpTestRunning)
			iS->iTestState = ESpTestIdle;
		iS->iMsg.Copy(m.Left(Min(m.Length(), iS->iMsg.MaxLength())));
		iS->iHelperSeq++;
		}
	SetLast(m);
	AppLog(m);
	HelperStopped();
	iEikonEnv->InfoMsg(m);
	}

// the helper has ended (or died): whatever the app was waiting for is over
void CSpAppUi::HelperStopped()
	{
	iTesting = EFalse;
	iChecking = EFalse;
	if (iS)
		{
		iS->iLogDone = iS->iLogReq;
		iS->iSetupDone = iS->iSetupReq;
		iS->iFreeDone = iS->iFreeReq;
		iS->iCheckDone = iS->iCheckReq;
		}
	if (iBusy != ESpIdle)
		{
		iBusy = ESpIdle;
		iEikonEnv->BusyMsgCancel();
		}
	RefreshHook();
	ButtonStates();
	}

void CSpAppUi::HelperMissing(TInt aErr)
	{
	TBuf<80> m;
	if (aErr == KErrNotReady)
		m = _L("Not available - an older helper is still running: restart the Psion");
	else if (aErr == KErrNotFound || aErr == KErrPathNotFound)
		{
		TFileName exe = AppDir();
		exe.Append(KSpHelperExe);
		m.Format(_L("Not found (%d): "), aErr);
		m.Append(exe.Right(Min(exe.Length(), 60)));
		}
	else
		m.Format(_L("PsiBoost's helper did not start (%d)"), aErr);
	AppLog(m);
	iEikonEnv->InfoMsg(m);
	}

// an infoprint, and ETrue, when the helper is busy with something else
TBool CSpAppUi::NotNow() const
	{
	const TText* why = NULL;
	if (iBusy == ESpBusySetup)
		why = iBusyOn ? _S("Not available - turning 230,400 bps always on") : _S("Not available - turning 230,400 bps always off");
	else if (iBusy == ESpBusyFree)
		why = _S("Not available - freeing the serial port");
	else if (iBusy == ESpBusyLog)
		why = _S("Not available - sending the log");
	else if (iTesting)
		why = _S("Not available - a test is running");
	if (why)
		iEikonEnv->InfoMsg(TPtrC(why));
	return why != NULL;
	}

// 230400 always on or off: done by the helper while a busy message shows
// (AlwaysDone)
void CSpAppUi::StartAlwaysL(TBool aOn)
	{
	if (NotNow())
		return;
	if (!iS)
		{
		HelperMissing(KErrNotReady);
		return;
		}
	if (aOn && !iHookOpen)
		{
		iEikonEnv->InfoMsg(iHookErr == KErrNotSupported ? _L("Not available - for a 5mx with ROM 1.05(260) only")
		                                                : _L("Not available - PsiBoost's driver did not load"));
		return;
		}
	iS->iAuto = iSettings.iAlways;
	iS->iNoModem = iSettings.iOtherEnd;
	iS->iSetupWant = aOn ? 1 : 0;
	iS->iSetupResult = ESpSetupNone;
	iS->iSetupReq++;
	TInt r = EnsureHelper();
	if (r != KErrNone)
		{
		iS->iSetupDone = iS->iSetupReq;
		HelperMissing(r);
		return;
		}
	iBusy = ESpBusySetup;
	iBusyOn = aOn;
	iEikonEnv->BusyMsgL(aOn ? _L("Turning 230,400 bps always on...") : _L("Turning 230,400 bps always off..."),
		EHLeftVBottom, TTimeIntervalMicroSeconds32(0));
	ButtonStates();
	iView->DrawNow();
	}

// the helper has turned 230400 always on or off, or could not: what it
// found, as an infoprint
void CSpAppUi::AlwaysDone()
	{
	iBusy = ESpIdle;
	iEikonEnv->BusyMsgCancel();
	TInt res = iS->iSetupResult;
	iSettings.iAlways = iS->iSetupNowOn ? 1 : 0;
	iS->iAuto = iSettings.iAlways;
	SaveSettings();
	RefreshHook();
	const TText* m;
	if (res == ESpSetupInUse)
		m = _S("The serial port is in use - Speed > Free the serial port");
	else if (res == ESpSetupForeign)
		m = _S("The serial port is hooked by another program - restart the Psion");
	else if (iBusyOn && NoModem() && res == ESpSetupOk)
		m = _S("230,400 bps always on - set the other end to 230,400 bps");
	else if (!iBusyOn && NoModem() && res == ESpSetupOk)
		m = _S("230,400 bps always off - set the other end back to 115,200 bps");
	else if (iBusyOn)
		switch (res)
			{
		case ESpSetupOk: m = _S("230,400 bps always on - 115,200 bps becomes 230,400 bps"); break;
		case ESpSetupNoModem: m = _S("No answer from the modem - it may be off or not connected"); break;
		case ESpSetupNoSpeed: m = _S("The modem cannot go to 230,400 bps - see PsiBoost.log"); break;
		case ESpSetupNoHook: m = _S("230,400 bps could not be turned on - see PsiBoost.log"); break;
		case ESpSetupNotHooked: m = _S("The port stayed at 115,200 bps - see PsiBoost.log"); break;
		case ESpSetupNoAnswer: m = _S("No answer from the modem at 230,400 bps - both back at 115,200 bps"); break;
		case ESpSetupNoSave: m = _S("The modem would not save 230,400 bps - both back at 115,200 bps"); break;
		default: m = _S("230,400 bps always is off - see PsiBoost.log"); break;
			}
	else if (res == ESpSetupOk)
		m = _S("230,400 bps always off - programs get the speed they ask for");
	else if (res == ESpSetupNoModem)
		m = _S("230,400 bps always off - no answer from the modem: set it back to 115,200 bps");
	else
		m = _S("230,400 bps always off - the modem may not be at 115,200 bps: see PsiBoost.log");
	ButtonStates();
	iView->DrawNow();
	// no modem answered: a cable to a computer, or a device whose speed is
	// set by hand? Asked, rather than an infoprint
	if (iBusyOn && res == ESpSetupNoModem && !NoModem())
		{
		AskNoModemL();
		return;
		}
	iEikonEnv->InfoMsg(TPtrC(m));
	}

// No modem answered when 230400 always was turned on: on Yes, it is turned
// on with no modem (the hook only, no AT), and PsiBoost.ini remembers there
// is none (Speed > The other end changes it back)
void CSpAppUi::AskNoModemL()
	{
	if (!iEikonEnv->QueryWinL(_L("No modem answered - for a computer, set its port to 230,400 bps"),
	                          _L("Turn on 230,400 bps always anyway?")))
		{
		iEikonEnv->InfoMsg(_L("230,400 bps always is off - no modem answered"));
		return;
		}
	iSettings.iOtherEnd = 1;
	SaveSettings();
	StartAlwaysL(ETrue);
	}

// Speed > The other end: a modem, or a computer or other device
void CSpAppUi::OtherEndL()
	{
	if (NotNow())
		return;
	TInt end = iSettings.iOtherEnd;
	CEikDialog* d = new(ELeave) CSpEndDialog(end, iSettings.iAlways, EFalse);
	if (!d->ExecuteLD(R_SP_END_DIALOG) || end == iSettings.iOtherEnd)
		return;
	SetOtherEnd(end);
	}

// the other end chosen (Speed > The other end, or the welcome): saved, the
// helper told, and the page's last line says what next
void CSpAppUi::SetOtherEnd(TInt aEnd)
	{
	iSettings.iOtherEnd = aEnd;
	SaveSettings();
	if (iS)
		{
		iS->iNoModem = aEnd;
		iS->iModemSpeed = KSpModemNotLooked;
		}
	if (!aEnd)
		StartCheck();
	SetLast(aEnd ? _L("The other end is a computer - PsiBoost sends it no AT commands")
	             : _L("The other end is a modem - PsiBoost tells it the speed"));
	ButtonStates();
	iView->DrawNow();
	}

// Speed > Free the serial port: Psion Internet stopped, then up to 20 s for the port
void CSpAppUi::StartFreeL()
	{
	if (NotNow())
		return;
	if (!iS)
		{
		HelperMissing(KErrNotReady);
		return;
		}
	iS->iFreeReq++;
	TInt r = EnsureHelper();
	if (r != KErrNone)
		{
		iS->iFreeDone = iS->iFreeReq;
		HelperMissing(r);
		return;
		}
	iBusy = ESpBusyFree;
	iEikonEnv->BusyMsgL(_L("Freeing the serial port..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(0));
	iView->DrawNow();
	}

void CSpAppUi::FreeDone()
	{
	iBusy = ESpIdle;
	iEikonEnv->BusyMsgCancel();
	iEikonEnv->InfoMsg(iS->iFreeResult ? _L("The serial port is free") : _L("The serial port is in use - close the program using it"));
	iView->DrawNow();
	}

// a quick look at the modem's speed (quiet: the hook's line shows it)
void CSpAppUi::StartCheck()
	{
	if (!iS || iChecking || EnsureHelper() != KErrNone)
		return;
	iChecking = ETrue;
	iS->iCheckReq++;
	}

// Tools > Test the speed: the dialog, then the helper runs it
void CSpAppUi::StartTestL()
	{
	if (NotNow())
		return;
	if (!iS)
		{
		HelperMissing(KErrNotReady);
		return;
		}
	if (iSettings.iTestRoute < 0)
		iSettings.iTestRoute = iRoute == 1 ? 1 : 0;   // (as Connection settings, the first time)
	TInt only = 0;
	CEikDialog* d = new(ELeave) CSpTestDialog(iSettings, only, NoModem(), iSettings.iAlways);
	if (!d->ExecuteLD(R_SP_TEST_DIALOG))
		return;
	SaveSettings();
	iTestOnly = only;
	iS->iTestOnly = only;
	{
	// (written by PsiBoost itself as well as by the helper: if the helper
	// never takes the test up, the log still says it was asked for)
	TBuf8<64> host;
	TInt base = 7100;
	SplitServer(iSettings.iTestHost, host, base);
	TBuf<64> h;
	h.Copy(host);
	TBuf<240> m(_L("Test the speed asked for: "));
	m.Append(only == 115200 ? _L("115,200 bps only") : only == 230400 ? _L("230,400 bps only") : _L("both speeds"));
	m.Append(iSettings.iTestRoute == 1 ? _L(", Psion Internet") : _L(", the modem route"));
	if (iSettings.iTestServer == ESpServerOwn)
		m.AppendFormat(_L(", your own server %S, base port %d"), &h, base);
	else
		m.Append(_L(", public servers"));
	m.AppendFormat(_L(", %d s each leg"), KTestSecs[iSettings.iTestLen >= 0 && iSettings.iTestLen <= 2 ? iSettings.iTestLen : 0]);
	AppLog(m);
	}
	// (the request first: a helper started now finds it waiting)
	iS->iAuto = iSettings.iAlways;
	iS->iTestRoute = iSettings.iTestRoute;
	iS->iTestServer = iSettings.iTestServer;
	iS->iTestSecs = KTestSecs[iSettings.iTestLen >= 0 && iSettings.iTestLen <= 2 ? iSettings.iTestLen : 0];
	{
	TBuf8<64> host;
	TInt base = 7100;
	SplitServer(iSettings.iTestHost, host, base);
	iS->iTestHost.Copy(host);
	iS->iTestBase = base;
	}
	iS->iTestStop = 0;
	iS->iTestNow.Zero();
	iS->iTestState = ESpTestRunning;      // (until the helper says otherwise)
	iS->iTestReq++;
	TInt r = EnsureHelper();
	if (r != KErrNone)
		{
		iS->iTestState = ESpTestIdle;
		TBuf<80> m;
		m.Format(_L("Test the speed: not run - PsiBoost's helper is not running (%d)"), r);
		AppLog(m);
		HelperMissing(r);
		return;
		}
	iTesting = ETrue;
	iView->ShowResults(EFalse);
	iView->DrawNow();
	ButtonStates();
	}

// now, as seconds since 1 Jan 2000 (local time), for the results' columns
static TTime Since2000() { return TTime(TDateTime(2000, EJanuary, 0, 0, 0, 0, 0)); }

static TInt NowSecs()
	{
	TTime now;
	now.HomeTime();
	TTimeIntervalSeconds secs;
	if (now.SecondsFrom(Since2000(), secs) != KErrNone || secs.Int() <= 0)
		return 0;
	return secs.Int();
	}

// "20 Oct 14:32"
static void WhenText(TDes& aText, TInt aSecs)
	{
	TTime t = Since2000();
	t += TTimeIntervalSeconds(aSecs);
	TDateTime d = t.DateTime();
	_LIT(KMonths, "JanFebMarAprMayJunJulAugSepOctNovDec");
	const TDesC& months = KMonths;
	TPtrC month = months.Mid(Min((TInt)d.Month(), 11) * 3, 3);
	aText.Format(_L("%d %S %02d:%02d"), d.Day() + 1, &month, d.Hour(), d.Minute());
	}

// the helper has finished a test: the results kept and shown. At one speed
// only, that column alone changes (the other keeps its last result), and the
// verdict compares the two when both have passed
void CSpAppUi::TestDone()
	{
	iTesting = EFalse;
	if (!iSettings.iResDone)
		{
		iSettings.iRes[0][0] = iSettings.iRes[1][0] = -1;
		iSettings.iResAt[0] = iSettings.iResAt[1] = 0;
		iSettings.iResPass = 0;
		}
	iSettings.iResDone = 1;
	TInt bits = iS->iTestPass, now = NowSecs();
	for (TInt h = 0; h < 2; h++)
		{
		if (iTestOnly && (iTestOnly == 230400) != (h == 1))
			continue;
		const TSpHalf& half = iS->iTestHalf[h];
		TInt* r = iSettings.iRes[h];
		r[0] = half.iDone ? (TInt)half.iDownRate : -1;
		r[1] = (TInt)half.iEchoRate;
		r[2] = (TInt)half.iRtt;
		r[3] = half.iStalls;
		r[4] = half.iBad;
		r[5] = half.iDrops;
		iSettings.iResAt[h] = half.iDone ? now : 0;
		TInt bit = 2 << h;
		iSettings.iResPass = (iSettings.iResPass & ~bit) | (bits & bit);
		}
	TBool pass = (bits & 1) != 0;
	iSettings.iVerdict.Copy(iS->iTestVerdict.Left(Min(iS->iTestVerdict.Length(), iSettings.iVerdict.MaxLength())));
	// one speed passed, and the other column passed last time: the two compared
	TInt d115 = iSettings.iRes[0][0], d230 = iSettings.iRes[1][0];
	if (iTestOnly && pass && (iSettings.iResPass & 6) == 6 && d115 > 0 && d230 > 0)
		{
		TInt x10 = d230 * 10 / d115;     // (bytes a second: no overflow)
		pass = d230 * 4 >= d115 * 5;     // (faster by a quarter at least, as the helper's verdict)
		if (pass)
			iSettings.iVerdict.Format(_L8("%d.%d x faster, no errors"), x10 / 10, x10 % 10);
		else
			iSettings.iVerdict.Format(_L8("FAIL: no faster at 230,400 bps (%d.%d x the 115,200 bps rate)"), x10 / 10, x10 % 10);
		}
	iSettings.iResPass = (iSettings.iResPass & ~1) | (pass ? 1 : 0);
	SaveSettings();
	{
	TBuf<220> m(_L("Test results shown: "));
	TBuf<200> v;
	v.Copy(iSettings.iVerdict);
	m.Append(v.Left(Min(v.Length(), m.MaxLength() - m.Length())));
	AppLog(m);
	}
	RefreshHook();
	if (!iTestOnly && !NoModem())
		StartCheck();                    // (the modem's speed after the test)
	iView->ShowResults(ETrue);
	iView->DrawNow();
	ButtonStates();
	}

// Tools > Send log to server: to the user's own PsiProxy, the proxy in
// Connection settings, as PsiTerm, PsiMail and PsiWeb send theirs
void CSpAppUi::SendLogL()
	{
	if (NotNow())
		return;
	if (!iS)
		{
		HelperMissing(KErrNotReady);
		return;
		}
	{
	TPsiLink link;
	link.SetDefaults();
	TBuf<64> host;
	TInt port = 0;
	if (!link.Load(iCoeEnv->FsSession()) || !link.ProxyUsable() || !link.DevBox(iCoeEnv->FsSession(), host, port))
		{
		AppLog(_L("Send log: not available - no proxy set in Connection settings"));
		iEikonEnv->InfoMsg(_L("Not available - set a proxy in Connection settings"));
		return;
		}
	TBuf<100> m;
	m.Format(_L("Send log asked for: to the proxy %S:%d"), &host, port);
	AppLog(m);
	}
	iS->iLogReq++;
	TInt r = EnsureHelper();
	if (r != KErrNone)
		{
		iS->iLogDone = iS->iLogReq;
		AppLog(_L("Send log: not sent - PsiBoost's helper is not running"));
		HelperMissing(r);
		return;
		}
	iBusy = ESpBusyLog;
	iEikonEnv->BusyMsgL(_L("Sending the log..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(300000));
	}

// up to aMax bytes from the end of aName, appended to aOut; ETrue if it was read
static TBool ReadTail(RFs& aFs, const TDesC& aName, TDes8& aOut, TInt aMax)
	{
	RFile f;
	if (f.Open(aFs, aName, EFileRead | EFileShareAny) != KErrNone)
		return EFalse;
	TInt size = 0;
	f.Size(size);
	TInt room = Min(aMax, aOut.MaxLength() - aOut.Length());
	TInt pos = size > room ? size - room : 0;
	f.Seek(ESeekStart, pos);
	TPtr8 rest((TUint8*)aOut.Ptr() + aOut.Length(), 0, room);
	TInt r = f.Read(rest, room);
	f.Close();
	if (r != KErrNone)
		return EFalse;
	aOut.SetLength(aOut.Length() + rest.Length());
	return ETrue;
	}

// Tools > Show log: the end of PsiBoost.log (and of PsiBoost.old before it,
// when the log has only just been started again), read only, in a dialog
void CSpAppUi::ShowLogL()
	{
	HBufC8* raw = HBufC8::NewLC(KShowLogBytes);
	TPtr8 d = raw->Des();
	TFileName name = AppDir();
	name.Append(KLogName);
	TFileName old = AppDir();
	old.Append(KOldLogName);
	RFs& fs = iCoeEnv->FsSession();
	TEntry e;
	TInt now = fs.Entry(name, e) == KErrNone ? e.iSize : 0;
	if (now < KShowLogBytes)
		ReadTail(fs, old, d, KShowLogBytes - now);
	ReadTail(fs, name, d, KShowLogBytes);
	if (!d.Length())
		{
		CleanupStack::PopAndDestroy();
		iEikonEnv->InfoMsg(_L("Nothing to show - the log is empty"));
		return;
		}
	// a cut first line is dropped; lines become paragraphs; other control
	// characters are left out
	TInt start = 0;
	if (d.Length() >= KShowLogBytes - 1)
		{
		TInt nl = d.Locate('\n');
		if (nl >= 0 && nl + 1 < d.Length())
			start = nl + 1;
		}
	HBufC* text = HBufC::NewLC(d.Length() - start + 1);
	TPtr t = text->Des();
	for (TInt i = start; i < d.Length(); i++)
		{
		TUint8 c = d[i];
		if (c == '\n')
			{
			if (i + 1 < d.Length())
				t.Append(CEditableText::EParagraphDelimiter);
			}
		else if (c >= 32 && c != 127)
			t.Append((TText)c);
		}
	CleanupStack::Pop();                 // (text: the dialog owns it now)
	CleanupStack::PopAndDestroy();       // raw
	(new(ELeave) CSpLogDialog(text))->ExecuteLD(R_SP_LOG_DIALOG);
	}

// The first time PsiBoost opens: the welcome (what it has been tested on,
// and to back up C: first), then what is on the other end of the serial
// cable. Neither page has Cancel, and Esc does not get out of them: closing
// the program from the list of open files and programs leaves PsiBoost.ini
// as it was, so they come again next time. Upgrading from 0.23 or earlier:
// the welcome and the question again, unless the other end is known
// already (a computer chosen, or 230,400 bps always on with a modem that
// answered). PsiBoost.ini field 1 becomes 2 once the other end is chosen.
void CSpAppUi::WelcomeL()
	{
	TBool ask = !iSettings.iOtherEnd && !iSettings.iAlways;
	if (!iSettings.iWarned || ask)
		(new(ELeave) CSpWelcomeDialog)->ExecuteLD(R_SP_WELCOME_DIALOG);
	if (ask)
		{
		TInt end = iSettings.iOtherEnd;
		(new(ELeave) CSpEndDialog(end, EFalse, ETrue))->ExecuteLD(R_SP_SETUP_DIALOG);
		iSettings.iWarned = 2;
		SetOtherEnd(end);                // (saves)
		return;
		}
	iSettings.iWarned = 2;
	SaveSettings();
	}

// Tools > Restart the Psion: a soft reset, as the reset button
void CSpAppUi::RestartL()
	{
	if (NotNow())
		return;
	TBool back = iSettings.iAlways && iHookOpen;
	if (!iEikonEnv->QueryWinL(back ? _L("Every program closes - 230,400 bps comes back on as the Psion starts")
	                               : _L("Every program closes - the files stay"), _L("Restart the Psion?")))
		return;
	SaveSettings();
	TInt r = UserSvr::ResetMachine(EStartupWarmReset);
	TBuf<80> m;                          // (only if it did not restart)
	m.Format(_L("Not restarted (%d) - use the reset button"), r);
	iEikonEnv->InfoMsg(m);
	}

TInt CSpAppUi::Tick(TAny* aThis)
	{
	((CSpAppUi*)aThis)->DoTick();
	return 0;
	}

void CSpAppUi::DoTick()
	{
	if (iWarnPending)
		{
		iWarnPending = EFalse;           // (first: the dialog runs the timer again)
		TRAPD(r, WelcomeL());
		(void)r;
		}
	TBool redraw = EFalse;
	if (++iTicks % KHookTicks == 0 && RefreshHook())
		{
		redraw = ETrue;
		ButtonStates();
		}
	if (!iS)
		{
		if (redraw)
			iView->DrawNow();
		return;
		}
	iS->iAppBeat++;
	if (iTicks % 10 == 0)
		ReadRoute();                     // (Connection settings may have changed: every 5 s)
	if (iHelperOpen && iHelper.ExitType() != EExitPending && iS->iState != ESpStopped)
		HelperDied();
	if (iS->iHelperSeq != iSeenSeq)
		{
		iSeenSeq = iS->iHelperSeq;
		redraw = ETrue;
		if (iS->iMsg.Length())
			{
			TBuf<100> m;
			m.Copy(iS->iMsg.Left(Min(iS->iMsg.Length(), m.MaxLength())));
			SetLast(m);
			}
		if (iS->iState == ESpStopped)
			{
			iHelperStarted = EFalse;
			if (iHelperOpen)
				{
				iHelper.Close();
				iHelperOpen = EFalse;
				}
			if (iTesting && iS->iTestState == ESpTestRunning)
				{
				iS->iTestState = ESpTestIdle;    // (the helper ended: no test)
				AppLog(_L("Test the speed: PsiBoost's helper ended before the test did"));
				}
			TBool waiting = iBusy != ESpIdle;
			HelperStopped();
			if (waiting && iS->iDriver != KErrNone)
				iEikonEnv->InfoMsg(_L("Not available - a device driver did not load"));
			}
		if (iBusy == ESpBusySetup && iS->iSetupDone == iS->iSetupReq)
			AlwaysDone();
		if (iBusy == ESpBusyFree && iS->iFreeDone == iS->iFreeReq)
			FreeDone();
		if (iChecking && iS->iCheckDone == iS->iCheckReq)
			iChecking = EFalse;
		if (iBusy == ESpBusyLog && iS->iLogDone == iS->iLogReq)
			{
			iBusy = ESpIdle;
			iEikonEnv->BusyMsgCancel();
			iEikonEnv->InfoMsg(iLast);
			TBuf<120> m(iS->iLogOk ? _L("Send log: sent - ") : _L("Send log: not sent - "));
			m.Append(iLast.Left(Min(iLast.Length(), m.MaxLength() - m.Length())));
			AppLog(m);
			}
		if (iTesting && iS->iTestState == ESpTestDone)
			TestDone();
		else if (iTesting && iS->iTestState == ESpTestIdle)
			iTesting = EFalse;
		}
	if (redraw)
		iView->DrawNow();
	}

// ----- what the page says -------------------------------------------------------

// a number with thousands commas (EPOC's Format has none), then aUnit:
// "1,234 ms"; negative numbers with a minus
static void Grouped(TDes& aText, TInt aV, const TDesC& aUnit = KNullDesC)
	{
	aText.Zero();
	TUint v = aV < 0 ? (TUint)-aV : (TUint)aV;
	TBuf<12> d;
	d.Num(v);
	if (aV < 0)
		aText.Append('-');
	for (TInt i = 0; i < d.Length(); i++)
		{
		if (i > 0 && (d.Length() - i) % 3 == 0)
			aText.Append(',');
		aText.Append(d[i]);
		}
	aText.Append(aUnit);
	}

static void Rate(TDes& aText, TInt aBytesPerSec)
	{
	if (aBytesPerSec < 0)
		aText = _L("-");
	else
		{
		Grouped(aText, aBytesPerSec / 1024);
		aText.AppendFormat(_L(".%d KB/s"), (aBytesPerSec % 1024) * 10 / 1024);
		}
	}

// how each of the page's choices is now, in a few words; aOn: on (the
// choice is pressed in, as a latched toolbar button); aDimmed: not available
void CSpAppUi::TileState(TInt aTile, TDes& aText, TBool& aOn, TBool& aDimmed) const
	{
	aOn = aDimmed = EFalse;
	aText.Zero();
	if (aTile == ESpTileTest)
		{
		aOn = iTesting;
		if (iTesting && iS)
			aText.Copy(iS->iTestNow.Left(Min(iS->iTestNow.Length(), aText.MaxLength())));
		else if (iSettings.iResDone && iSettings.iVerdict.Length())
			{
			aText.Copy(iSettings.iVerdict.Left(Min(iSettings.iVerdict.Length(), aText.MaxLength())));
			TInt cut = aText.Find(_L(" ["));   // (the step that failed: the results page has it)
			if (cut > 0)
				aText.SetLength(cut);
			}
		else
			aText = _L("Not tested yet");
		// (your own server: which, so it can be seen before a test)
		if (!iTesting && iSettings.iTestServer == ESpServerOwn && iSettings.iTestHost.Length())
			{
			TBuf<64> h;
			h.Copy(iSettings.iTestHost.Left(Min(iSettings.iTestHost.Length(), h.MaxLength())));
			if (aText.Length() + 14 + h.Length() <= aText.MaxLength())
				{
				aText.Append(_L(" - server "));
				aText.Append(h);
				}
			}
		return;
		}
	if (aTile == ESpTileFree)
		{
		aOn = iBusy == ESpBusyFree;
		aText = aOn ? _L("Waiting for the port...") : _L("Stops Psion Internet and waits for the port");
		return;
		}
	aOn = iSettings.iAlways;
	if (iBusy == ESpBusySetup)
		{
		aOn = iBusyOn;
		aText = iBusyOn ? _L("Turning on...") : _L("Turning off...");
		}
	else if (!iHookOpen)
		{
		aDimmed = ETrue;
		aText = _L("Not available on this Psion");
		}
	else
		aText = iSettings.iAlways ? _L("On - tap to turn off") : _L("Off - tap to turn on");
	}

// a choice tapped, or Enter on it
void CSpAppUi::TileActL(TInt aTile)
	{
	if (aTile == ESpTileAlways)
		StartAlwaysL(!iSettings.iAlways);
	else if (aTile == ESpTileFree)
		StartFreeL();
	else if (iTesting)
		iEikonEnv->InfoMsg(_L("A test is running - Tools > Stop the test"));
	else
		StartTestL();
	}

// The status: aAlways "230400 always: on" or off; aHook the hook now (and
// the modem, when the quick look says it does not match); aLast the last
// thing that happened
void CSpAppUi::StatusTexts(TDes& aAlways, TDes& aHook, TDes& aLast) const
	{
	aAlways = iSettings.iAlways ? _L("230,400 bps always: on") : _L("230,400 bps always: off");
	aLast = iLast;
	TInt modem = iS ? iS->iModemSpeed : KSpModemNotLooked;
	if (!iHookOpen)
		{
		if (iHookErr == KErrNotSupported)
			aHook = _L("Hook: not available - for a 5mx with ROM 1.05(260) only");
		else
			aHook.Format(_L("Hook: not available - its driver did not load (%d)"), iHookErr);
		}
	else if (iHookFound == EPbHooked && NoModem())
		aHook = _L("Hook: on - the other end must be at 230,400 bps");
	else if (iHookFound == EPbStock && NoModem())
		aHook = _L("Hook: off - the other end must be at 115,200 bps");
	else if (iHookFound == EPbHooked)
		aHook = modem == 115200 ? _L("Hook: on but the modem is at 115,200 bps") : _L("Hook: on - 115,200 bps becomes 230,400 bps");
	else if (iHookFound == EPbStock)
		aHook = modem == 230400 ? _L("Hook: off but the modem is at 230,400 bps") : _L("Hook: off");
	else if (iHookFound == EPbForeign)
		aHook = _L("Hook: taken by another program - restart the Psion");
	else
		aHook = _L("Hook: off - the serial port's driver was not found");
	if (!aLast.Length() && modem == 0 && iS && !NoModem())
		aLast = _L("The modem did not answer when PsiBoost looked");
	}

// the results page's words: the verdict, then five rows of three cells
void CSpAppUi::ResultTexts(TDes& aVerdict, TSpCell* aCells, TBool& aPass) const
	{
	aPass = (iSettings.iResPass & 1) != 0;
	aVerdict.Copy(iSettings.iVerdict.Left(Min(iSettings.iVerdict.Length(), aVerdict.MaxLength())));
	if (!iSettings.iResDone)
		aVerdict = _L("Not tested yet");
	for (TInt i = 0; i < KResCells; i++)
		aCells[i] = _L("-");
	if (!iSettings.iResDone)
		return;
	const TInt* a = iSettings.iRes[0];
	const TInt* b = iSettings.iRes[1];
	TBool ranA = a[0] >= 0, ranB = b[0] >= 0;
	if (ranA) Rate(aCells[0], a[0]);
	if (ranB) Rate(aCells[1], b[0]);
	if (ranA) Rate(aCells[3], a[1]);
	if (ranB) Rate(aCells[4], b[1]);
	_LIT(KMs, " ms");
	if (ranA && a[2]) Grouped(aCells[6], a[2], KMs);
	if (ranB && b[2]) Grouped(aCells[7], b[2], KMs);
	if (ranA) Grouped(aCells[9], a[3]);
	if (ranB) Grouped(aCells[10], b[3]);
	if (ranA) Grouped(aCells[12], a[4]);
	if (ranB) Grouped(aCells[13], b[4]);
	// the gain: x.y times, for the rates
	for (TInt row = 0; row < 2; row++)
		{
		TInt x = a[row], y = b[row];
		if (ranA && ranB && x > 0 && y > 0)
			aCells[row * 3 + 2].Format(_L("%d.%d x"), y / x, (y % x) * 10 / x);
		}
	if (ranA && ranB && a[2] > 0 && b[2] > 0)
		Grouped(aCells[8], b[2] - a[2], KMs);
	// (0.24) when each column was run
	if (ranA && iSettings.iResAt[0])
		WhenText(aCells[15], iSettings.iResAt[0]);
	if (ranB && iSettings.iResAt[1])
		WhenText(aCells[16], iSettings.iResAt[1]);
	aCells[17].Zero();
	}

// ----- commands -------------------------------------------------------------------

void CSpAppUi::ButtonStates()
	{
	if (!iToolBar)
		return;
	CEikButtonBase* b = (CEikButtonBase*)iToolBar->ControlById(ESpCmdFast);
	if (b)
		{
		TBool on = iBusy == ESpBusySetup ? iBusyOn : iSettings.iAlways;
		b->SetState(on ? CEikButtonBase::ESet : CEikButtonBase::EClear);
		b->DrawNow();
		}
	}

void CSpAppUi::DynInitMenuPaneL(TInt aResourceId, CEikMenuPane* aMenuPane)
	{
	TBool busy = iBusy != ESpIdle || iTesting;
	if (aResourceId == R_SP_TOOLS_MENU)
		{
		aMenuPane->SetItemDimmed(ESpCmdTest, busy);
		aMenuPane->SetItemDimmed(ESpCmdTestStop, !iTesting);
		aMenuPane->SetItemDimmed(ESpCmdResults, !iSettings.iResDone);
		aMenuPane->SetItemDimmed(ESpCmdSendLog, busy);
		aMenuPane->SetItemDimmed(ESpCmdRestart, busy);
		}
	else if (aResourceId == R_SP_SPEED_MENU)
		{
		aMenuPane->SetItemButtonState(ESpCmdFast, iSettings.iAlways ? EEikMenuItemSymbolOn : EEikMenuItemSymbolIndeterminate);
		aMenuPane->SetItemDimmed(ESpCmdFast, busy || !iHookOpen);
		aMenuPane->SetItemDimmed(ESpCmdFree, busy);
		aMenuPane->SetItemDimmed(ESpCmdCheck, busy);
		aMenuPane->SetItemDimmed(ESpCmdOtherEnd, busy);
		}
	else if (aResourceId == R_SP_VIEW_MENU)
		aMenuPane->SetItemButtonState(ESpCmdToolbar, iSettings.iToolbar ? EEikMenuItemSymbolOn : EEikMenuItemSymbolIndeterminate);
	}

void CSpAppUi::HandleCommandL(TInt aCommand)
	{
	switch (aCommand)
		{
	case ESpCmdFast:
		StartAlwaysL(!iSettings.iAlways);
		ButtonStates();                  // (a latched button tapped while busy goes back)
		break;
	case ESpCmdFree:
		StartFreeL();
		break;
	case ESpCmdCheck:
		if (NoModem())
			iEikonEnv->InfoMsg(_L("Not available - the other end is not a modem (Speed > The other end)"));
		else if (!NotNow())
			{
			if (iS)
				iS->iModemSpeed = KSpModemNotLooked;
			StartCheck();
			iEikonEnv->InfoMsg(iChecking ? _L("Looking at the modem") : _L("Not available - PsiBoost's helper is not running"));
			}
		break;
	case ESpCmdTest:
		StartTestL();
		break;
	case ESpCmdOtherEnd:
		OtherEndL();
		break;
	case ESpCmdTestStop:
		if (iTesting && iS)
			{
			iS->iTestStop = 1;
			iEikonEnv->InfoMsg(_L("Stopping the test"));
			}
		else
			iEikonEnv->InfoMsg(_L("Nothing to stop - no test running"));
		break;
	case ESpCmdResults:
		if (iSettings.iResDone)
			iView->ShowResults(!iView->Results());
		else
			iEikonEnv->InfoMsg(_L("No results yet - Tools > Test the speed"));
		break;
	case ESpCmdSendLog:
		SendLogL();
		break;
	case ESpCmdShowLog:
		ShowLogL();
		break;
	case ESpCmdRestart:
		RestartL();
		break;
	case ESpCmdToolbar:
		iSettings.iToolbar = !iSettings.iToolbar;
		SaveSettings();
		if (iToolBar)
			iToolBar->MakeVisible(iSettings.iToolbar);
		iView->SetRectL(ViewRect());
		iView->DrawNow();
		break;
	case ESpCmdHelp:
		(new(ELeave) CSpHelpDialog)->ExecuteLD(R_SP_HELP_DIALOG);
		break;
	case ESpCmdAbout:
		{
		TBuf<40> always;
		TBuf<100> hook, last;
		StatusTexts(always, hook, last);
		TBuf<70> s(hook.Left(Min(hook.Length(), 70)));
		(new(ELeave) CSpAboutDialog(s))->ExecuteLD(R_SP_ABOUT_DIALOG);
		break;
		}
	case EEikCmdExit:
		SaveSettings();
		Exit();
		break;
		}
	}

// ===========================================================================
// DLL
// ===========================================================================

EXPORT_C CApaApplication* NewApplication()
	{
	return new CSpApplication;
	}

GLDEF_C TInt E32Dll(TDllReason)
	{
	return KErrNone;
	}
