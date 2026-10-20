# How PsiBoost works

PsiBoost makes every program that asks the Psion Series 5mx's RS-232 port for 115,200 bps get 230,400 bps instead, from the first byte, with nothing running in the background. This page explains how, and what it writes. Everything here is for the 5mx ROM **1.05(260)** (English); the addresses are that ROM's.

## Why the Psion stops at 115,200 bps

A program reaches the serial port through this chain:

```
RComm (c32.dll)                  a program: PsiTerm, PsiMail, Psion Internet's PPP...
  -> C32, the comm server
     -> ECUART.CSY                 COMM::0 is UART2
        -> ECOMM.LDD               the logical device
           -> EUART2.PDD           the physical device, "Comm.Wind2"
              -> UART2's registers  UBRCR, the baud divider, at virtual 0x58000708
```

The UART itself can run at 230,400 bps: its divider is `UBRCR = 7372800 / (16 x rate) - 1`, so 3 is 115,200 bps and 1 is 230,400 bps. But EUART2's rate table stops at 115,200 bps, and anything faster is refused with `KErrNotSupported` (-5). So a program cannot ask for 230,400 bps, however it tries.

## The hook

EUART2's configure function (`0x5008aedc` in ROM) looks the rate up and writes the divider. Every serial channel reaches it through its vtable, and every channel is made by the "Comm.Wind2" factory object, a kernel object whose vtable is at `0x5008b61c`.

ROM is read-only, so PsiBoost's hook driver (`PBHOOK20.LDD`, `hook/pbhook.cpp`) keeps two constant copies of those vtables in its own code, each with one slot changed:

- the factory's **create** slot (+0x34) calls the real create, then points the new channel at the copied channel vtable;
- the channel's **configure** slot (+0x28) calls the real configure, which programs the port exactly as before, then, if the divider it wrote is 3 (115,200 bps), writes 1 (230,400 bps). Every other speed is left alone.

**Installing the hook is one word**: the factory object's vtable pointer is pointed at the copy. Removing it points it back. Ports opened while the hook was in keep working, because the copies live as long as the driver, which is never unloaded. The hook is in RAM, so a restart takes it out.

The factory object is not at a fixed address: it is found by walking the kernel's physical-device container (pointer at `0x80000998`) with guarded reads, for the object whose vtable is the stock one (`pbhook.h`, `PbFindFactory`).

### What is written, and the checks first

The hook driver writes kernel memory in exactly one place: the vtable pointer of the serial driver's factory object. Before it does, it checks:

- that the factory's vtable pointer is the stock ROM one (or its own copy, if the hook is already in);
- that the two ROM slots it wraps hold the addresses it expects (`0x5008aafc` and `0x5008aedc`).

If any of these differ, it writes nothing and says so. Its other requests only read: words in ROM or kernel data, bounded to those ranges.

Separately, the helper uses a small UART driver (`PBUART21.LDD`, `uart/pbuart.cpp`) to read UART2's registers and, only while its own port is open and the UART is on, to set the divider to 1 or 3 (to find which speed the modem answers at). It saves the value it found and puts it back when its port closes.

### Other ROMs

The program, the helper and the start-up recogniser all check the machine first: a 640x240 screen and ROM 1.05(260). On anything else they load no driver and write nothing, and 230,400 bps always is not available. The ordinals in `ekern_rom.def` and the addresses in `pbhook.h` are this ROM's; another ROM would need them read again.

## The modem

The Psion and the modem must agree on the speed. Turning 230,400 bps always on, the helper:

1. finds the modem at 115,200 bps (or at 230,400 bps, if it is there already);
2. tells it 230,400 bps with `AT$SB=230400`, `ATB230400` or `AT+IPR=230400`, whichever it takes;
3. closes the port, puts the hook in, opens the port again (now really at 230,400 bps; on a 5mx the divider is read back and must be 1);
4. checks `AT`, and saves the speed on the modem with `AT&W`.

If any step fails, the hook comes out and the modem is put back at 115,200 bps. Turning it off is the same the other way. Every command and answer goes to `PsiBoost.log`.

### With no modem

When nothing answers `AT`, PsiBoost asks whether to turn 230,400 bps always on anyway, for a cable straight to a computer or a device whose speed is set by hand. On Yes, field 2 of `PsiBoost.ini` (spare until 0.23) says the other end is not a modem, and the app passes that to the helper in the shared memory's `iNoModem` (once `iSpare1`). From then on turning it on or off only puts the hook in or takes it out: no `AT`, no `+++`. On a 5mx, if the port is free, the helper opens it once to read the divider back (1 means the hook took); opening it sends one CR. The quick look at the modem's speed is not made, and the link test is not available, as each half would have to change the other end's speed. **Speed > The other end** changes it back, while 230,400 bps always is off. The start-up recogniser reads only field 0, so it needs no change.

## At start-up

A restart takes the hook out, so PsiBoost installs a file recogniser, `PsiBoost.rdl`, in `D:\System\Recogs\` on the CF card. Apparc's server loads every recogniser in `\System\Recogs\` on every disk at start-up (`CApaScanningFileRecognizer::ScanForRecognizersL`, in the SDK's Apparc source) and calls its ordinal 1. PsiBoost's recognises nothing: it does its work there and returns NULL, so Apparc unloads it again. The work:

- nothing at all if `D:\System\Data\PsiBoost.off` (or `C:`'s) exists, or if Ctrl is held down;
- nothing unless this is a 5mx with ROM 1.05(260);
- nothing unless `PsiBoost.ini` says 230,400 bps always is on;
- then the hook driver is loaded, the factory found, and the hook installed with the checks above.

It writes one line to `PsiBoost.log`. It is on the CF card only, so taking the card out and restarting means nothing of PsiBoost's runs at start-up.

## The kernel classes

The SDK has no kernel headers. `kern.h` declares the two classes a logical device driver needs, `DLogicalDevice` and `DLogicalChannel`, rebuilt from the ROM's own `Video.ldd` and the mangled names in the SDK's `ekern.lib`, and checked against a disassembly of the ROM. The SDK's `ekern.lib` is for the Series 5 kernel, whose ordinals differ, so the drivers link against `ekern_rom.def`, which has the 5mx ROM's.
