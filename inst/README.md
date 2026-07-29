# inst/ — the IRIX Software Manager product description

These two files describe the `irixscsitb` product that IRIX's `inst`(1M) /
Software Manager (swmgr) installs. `scripts/iris-gendist.sh` ships them into
an IRIX 5.3 guest and runs the native **`gendist`** there (5.3-format product
files read by every inst from 5.3 through 6.5), producing the classic trio:

    irixscsitb  irixscsitb.idb  irixscsitb.sw

- `irixscsitb.spec` — product → image → subsystem tree. `@VERSION@` is
  replaced with a numeric version derived from the release version (digits,
  first 10 — date-stamped versions give sane inst upgrade ordering).
  `sw.o32` is the default-install subsystem (runs on 5.3–6.5); `sw.n32`
  (6.x-only, faster) is opt-in — both install the same paths, so inst treats
  selecting both as a conflict and makes the user pick one, which is the
  intended either/or.
- `irixscsitb.idb` — the file list, **sorted by destination path** (gendist
  requires it). Source paths are relative to the gendist `-sbase`, matching
  the `dist53/` / `dist65/` layout the work disk carries. Lines whose source
  binary wasn't built (e.g. a GUI-less image) are filtered out at staging
  time by iris-gendist.sh.

The tools come from the **`inst_dev.sw`** subsystem ("Software Packager") of
the IRIS Development Option 5.3 CD; iris-gendist.sh auto-installs it into the
guest's copy-on-write overlay when missing (see `IRIX53_IDO_ISO` in
ci/local.conf.example).
