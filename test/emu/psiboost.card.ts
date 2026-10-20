// PsiBoost's cards for the emulator, loaded by core/tools/emu/mkcard.ts
// from the checkout's test/emu/:
//   node --experimental-strip-types core/tools/emu/mkcard.ts psiboost [out.img]
// The files come from build/pkg/ beside test/ (build.sh).
import { fileURLToPath } from 'node:url';
const ROOT = fileURLToPath(new URL('../../', import.meta.url));

// PsiBoost.ini (version 4): 24 32-bit fields, then your own server and the
// last verdict as length-prefixed strings. The fields: 230400 always, the
// warning shown, spare, toolbar, testRoute, testServer, testLen, spare,
// resDone, resPass, 12 results, two spare.
const ini = (always: number, testRoute: number, testServer: number, host: string, warned = 1,
             res: number[] = [], verdict = '') => {
	const f = [always, warned, 0, 1, testRoute, testServer, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
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
	// recogniser: the hook is installed by the program. The warning has been
	// seen (as after the first time).
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
	// ...as installed, never opened: the first-time warning shows
	psiboostnew: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(0, 0, 0, '', 0));
	},
	// PsiBoost with 230400 always on in PsiBoost.ini, but no recogniser: the
	// program finds the hook off and puts it back (and says so)
	psibooston: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(1, 0, 0, ''));
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
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(1, 1, 0, '', 1, REAL, '1.4 x faster, no errors'));
	},
	// PsiBoost with its settings set for the link test against hostmodem and
	// server/stdsvc.py on the PC: the modem route, your own server 127.0.0.1
	// (stdsvc.py's standard base, 7100), quick
	psiboosttest: (c: any) => {
		cards.psiboost(c);
		c.w('SYSTEM/APPS/PSIBOOST/PSIBOOST.INI', ini(0, 0, 1, '127.0.0.1'));
	},
};
