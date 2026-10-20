// PsiBoost's cards for the emulator, loaded by core/tools/emu/mkcard.ts
// from the checkout's test/emu/:
//   node --experimental-strip-types core/tools/emu/mkcard.ts psiboost [out.img]
// The files come from build/pkg/ beside test/ (build.sh).
import { fileURLToPath } from 'node:url';
const ROOT = fileURLToPath(new URL('../../', import.meta.url));

// PsiBoost.ini (version 4): 24 32-bit fields, then your own server and the
// last verdict as length-prefixed strings. The fields: 230400 always, the
// welcome (0 not shown, 1 the warning of 0.21 to 0.23, 2 the 0.24 welcome
// and the other end chosen), the other end (0.23: 0 a modem, 1 a computer),
// toolbar, testRoute, testServer, testLen, the test's speeds (0.24: 2 both),
// resDone, resPass (0.24: bit 0 the verdict, bits 1 and 2 each column), 12
// results, when each column was run (0.24).
const ini = (always: number, testRoute: number, testServer: number, host: string, warned = 2,
             res: number[] = [], verdict = '', end = 0) => {
	const f = [always, warned, end, 1, testRoute, testServer, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
	res.forEach((v, i) => { f[8 + i] = v; });
	const b = new Uint8Array(8 + 4 * f.length + 1 + host.length + 1 + verdict.length);
	const dv = new DataView(b.buffer);
	dv.setInt32(0, 4, true);
	dv.setInt32(4, f.length, true);
	f.forEach((v, i) => dv.setInt32(8 + 4 * i, v, true));
	let at = 8 + 4 * f.length;
	b[at++] = host.length;
	for (let i = 0; i < host.length; i++) b[at++] = host.charCodeAt(i);
	b[at++] = verdict.length;
	for (let i = 0; i < verdict.length; i++) b[at++] = verdict.charCodeAt(i);
	return b;
};

// A real 5mx's test (19 Oct 2026: Psion Internet, public servers, 15 s legs),
// for the screenshots: done, passed, then at 115200 and at 230400 the
// download and echo in bytes a second, the round trip in ms, stalls, bad
// bytes, drops
const REAL = [1, 1, 4772, 3926, 234, 0, 0, 0, 7066, 5392, 203, 0, 0, 0];

export const cards = {
	// PsiBoost: the program first on the card, so the harness opens it. No
	// recogniser: the hook is installed by the program. The welcome has been
	// seen and the other end is a modem (as after the first time).
	psiboost: (c: any) => {
		const P = c.pkg(ROOT + 'build/pkg/');
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.APP', c.f(P + 'psiboost.app'));
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.RSC', c.f(P + 'psiboost.rsc'));
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.AIF', c.f(P + 'psiboost.aif'));
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.MBM', c.f(P + 'psiboost.mbm'));
		c.w('SYSTEM/APPS/PSIBOOST/PBOOSTD.EXE', c.f(P + 'pboostd.exe'));
		c.w('SYSTEM/LIBS/PBUART21.LDD', c.f(P + 'pbuart21.ldd'));
		c.w('SYSTEM/LIBS/PBHOOK20.LDD', c.f(P + 'pbhook20.ldd'));
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(0, 0, 0, ''));
	},
	// ...as installed, never opened: the welcome and the other end's question
	// show, and Esc does not get past them
	psiboostnew: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(0, 0, 0, '', 0));
	},
	// ...as 0.23 left it: the warning seen, the other end not chosen (a
	// modem by default) and 230400 always off: 0.24 shows the welcome and
	// asks. (A 0.23 PsiBoost.ini with a computer chosen, or 230400 always
	// on, is not asked again: psiboostnomodem, psibooston.) The old field 7
	// of 0.10 to 0.19 is 1 here, as it may be on a real Psion.
	psiboost023: (c: any) => {
		cards.psiboost(c);
		const b = ini(0, 0, 0, '', 1);
		new DataView(b.buffer).setInt32(8 + 4 * 7, 1, true);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', b);
	},
	// PsiBoost with 230400 always on in PsiBoost.ini, but no recogniser: the
	// program finds the hook off and puts it back (and says so)
	psibooston: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(1, 0, 0, ''));
	},
	// 230400 always on with no modem (a cable to a computer): PsiBoost puts
	// the hook back and sends no AT. Turning it on with no modem, on the
	// plain psiboost card: test/emu/boot.sh with EMU_SERIAL bridged to
	// something that never answers (the emulator's port sends nothing with
	// nothing attached), the Remote link off, then a tap on the choice.
	psiboostnomodem: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(1, 0, 0, '', 1, [], '', 1));   // (as 0.23 left it)
	},
	// The start-up test: 230400 always on, and the recogniser in
	// D:\System\Recogs\ (as PsiBoost.sis installs it), so the hook is in
	// before the program is opened. Boot with the card in (test/emu/boot.sh).
	psiboostboot: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(1, 0, 0, ''));
		c.w('SYSTEM/RECOGS/PSIBOOST.RDL', c.f(c.pkg(ROOT + 'build/pkg/') + 'psiboost.rdl'));
	},
	// ...and with D:\System\Data\PsiBoost.off: the recogniser does nothing
	psiboostoff: (c: any) => {
		cards.psiboostboot(c);
		c.w('SYSTEM/DATA/PSIBOOST.OFF', c.t('off'));
	},
	// For the screenshots: as psiboostboot (boot with test/emu/boot.sh), and
	// a real 5mx's last test
	psiboostshots: (c: any) => {
		cards.psiboostboot(c);
		const b = ini(1, 1, 0, '', 2, REAL, '1.4 x faster, no errors');
		const dv = new DataView(b.buffer);
		dv.setInt32(8 + 4 * 9, 7, true);                 // (0.24: the verdict and both columns passed)
		dv.setInt32(8 + 4 * 22, 845733900, true);        // (when: 19 Oct 2026, 14:05 and 14:07)
		dv.setInt32(8 + 4 * 23, 845734020, true);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', b);
	},
	// PsiBoost with its settings set for the link test against hostmodem and
	// server/stdsvc.py on the PC: the modem route, your own server 127.0.0.1
	// (stdsvc.py's standard base, 7100), quick; both speeds last time (the
	// dialog offers Both first)
	psiboosttest: (c: any) => {
		cards.psiboost(c);
		const b = ini(0, 0, 1, '127.0.0.1');
		new DataView(b.buffer).setInt32(8 + 4 * 7, 2, true);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', b);
	},
	// ...and at one speed only (0.24): 115,200 bps, as 230400 always is off,
	// with an older result in the 230,400 bps column to keep
	psiboosttestone: (c: any) => {
		cards.psiboost(c);
		const old = [0, 0, 0, 0, 0, 0, 7066, 5392, 203, 0, 0, 0];
		const b = ini(0, 0, 1, '127.0.0.1', 2, [1, 5, ...old], 'An older test');
		new DataView(b.buffer).setInt32(8 + 4 * 10, -1, true);    // (no 115200 result)
		new DataView(b.buffer).setInt32(8 + 4 * 23, 846000000, true);   // (230400's: 23 Oct 2026)
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', b);
	},
};
