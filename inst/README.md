# inst/ — the IRIX Software Manager product description

These two templates describe the `irixscsitb` product that IRIX's `inst`(1M)
/ Software Manager (swmgr) installs. **Each OS packages its own build**:
during the build session, `scripts/iris-build.sh` runs the guest's native
`gendist` over these files, so the 5.3 guest emits a 5.3-format product for
its o32 binaries (readable by every inst from 5.3 through 6.5) and the 6.5
guest a 6.5-format product for its n32 binaries. The two land on the media as
`/dist53` and `/dist65` — `inst -f /CDROM/dist53` (or `dist65`) — and as the
per-flavor `.tardist` release artifacts.

- `irixscsitb.spec` — product → image → one subsystem per product.
  Placeholders filled at staging time (`stage_inst_inputs` in
  `scripts/ci-lib.sh`): `@VERSION@` (numeric, derived from the release
  version — date-stamped versions give sane inst upgrade ordering),
  `@SUBSYS@` (`o32`/`n32`), `@ABI_DESC@`. Both flavors' products share the
  product name, so installing the other flavor later simply replaces it.
- `irixscsitb.idb` — the file list, **sorted by destination path** (gendist
  requires it), sources relative to the gendist `-sbase`: the binaries under
  `bin/` plus the **Toolchest entry** `desktop/scsitoolbox.chest` →
  `/usr/lib/X11/app-chests/` (a drop-in fragment — no system file edited;
  its `f.checkexec.sh` verb hides the menu item whenever the GUI is absent,
  so removal needs no hook). When the GUI wasn't built (a Motif-less image),
  both the GUI line and the chest line are filtered out in-guest before
  gendist runs. The desktop icon (FTR rules + type-DB rebuild) remains the
  domain of the hand-install `install.sh` — activating it requires editing
  the filetype Makefile and running make there, which a package shouldn't do
  silently.

`gendist` ships in the **`inst_dev.sw`** subsystem ("Software Packager"). If
a 5.3 image lacks it, `scripts/iris-gendist.sh` can install it into the
guest's copy-on-write overlay from the IRIS Development Option 5.3 CD
(`IRIX53_IDO_ISO` in ci/local.conf) — that scripted session never quits
inst, never resolves conflicts, and never removes anything: after the
install reports success, inst is simply suspended and left behind in the
disposable overlay.
