# Building IRIX software in CI with the IRIS emulator

This repo doubles as a **sample project** for compiling native IRIX software on
modern infrastructure — a GitHub-hosted runner, a self-hosted runner, or any
Linux/macOS box — by booting a real IRIX installation inside the
[IRIS emulator](https://github.com/danifunker/iris) and driving its native
MIPSpro toolchain over the serial console. No cross-compiler, no networking
inside the guest, and the boot disk image is never modified.

```
host (Linux/macOS, CI runner)
│
├─ iris + iris-ci ................ prebuilt release pair (scripts/fetch-iris.sh)
│                                  or built from source with --features chd
├─ boot disk  <image>.chd ........ YOUR installed IRIX 5.3 / 6.5 dev system
│     └─ overlay = true .......... guest writes go to <image>.chd.diff.chd;
│                                  delete it to reset ("copy-on-write")
├─ work disk  work.hda ........... built per run by rb-cli from the staged
│     ├─ *.c *.h Makefile ........ sources IN   (new hd sgi-efs --from-dir)
│     └─ out/ .................... binaries OUT (rb-cli get work.hda@1 ...)
│
└─ scripts/iris-build.sh ......... boots the guest headless, drives the PROM
                                   and shell over the CI serial socket, runs
                                   the native make, extracts the binaries
```

## Quick start (local)

```sh
# once: get the emulator (prebuilt) and rb-cli on PATH
scripts/fetch-iris.sh --dir ../iris          # or: cd ../iris && cargo build --release --features chd,jit,rex-jit,lightning

# once: tell the scripts where YOUR boot disks live (gitignore'd, per-machine)
cp ci/local.conf.example ci/local.conf
$EDITOR ci/local.conf                 # set IRIX53_IMAGE / IRIX65_IMAGE

# o32 binaries (IRIX 5.3 guest; runs on 5.3-6.5) + .iso/.hda/.tar.gz packaging
scripts/iris-build.sh --flavor o32 --version 2026-07-28

# n32 binaries (IRIX 6.5 guest; 6.x only, faster; binaries only)
scripts/iris-build.sh --flavor n32
```

`ci/local.conf` is the one per-machine config (parsed KEY=VALUE, never
sourced; precedence everywhere is flag > environment variable > conf):
`IRIX53_IMAGE`/`IRIX65_IMAGE` (local disks), `IRIX53_DISK_URL`/
`IRIX65_DISK_URL` (private download URLs instead), `BUILD_O32`/`BUILD_N32`
(disable a flavor), `IRIS_DIR`, `IRIS_RELEASE_REPO`/`IRIS_TAG`, `RB_CLI` —
the `.example` documents each. Add `--fresh` to reset the guest to its
pristine state first (deletes the overlay). Everything the run used — machine
config, NVRAM, serial console log, work disk — is kept in the printed work
dir for inspection.

## What your boot image needs

| Requirement | Why |
|---|---|
| Installed IRIX 5.3–6.5 with cc, make, headers | it *is* the build machine |
| Motif dev tree (`/usr/include/Xm`) | only for the GUI; CLI builds without it |
| Root with an **empty password** | the serial login (else `IRIX_ROOT_PASSWORD`) |
| Everything on one partition (or adapt the mount step) | 5.3 builds run single-user |
| — no network config, no DHCP, no NVRAM MAC | media transfer doesn't need any |

The 5.3 flavor boots **single-user** (`sash` → `unix initstate=s`), so no rc2
service ever runs — third-party daemons that block a headless multiuser boot
(a tgcware `prngd` gathering entropy in the foreground, `xdm` respawning on a
machine with no graphics head) simply never start. The 6.5 flavor boots
multiuser, which the stock 6.5.22 rc set handles cleanly headless.

## Keeping the image fresh (copy-on-write)

`ci/iris-irix53.toml` and `ci/iris-irix65.toml` set `overlay = true` on the
boot disk: iris sends every guest write to an uncompressed `.diff.chd` sidecar
and **never folds it back into the base**. The base CHD stays byte-identical
(verified by SHA-1 in bring-up), so one downloaded/cached image serves any
number of runs; `--fresh` (or deleting the sidecar) rolls the guest back.
Without the flag iris would *fold the diff into a compressed base on clean
exit* — a slow rewrite of exactly the file CI wants immutable.

## Moving files in and out — the options

| Channel | Direction | Guest prereqs | Notes |
|---|---|---|---|
| **Work disk** (default) | both | none | `rb-cli new hd sgi-efs --from-dir`; guest mounts `/dev/dsk/dks0d2s0`; results read back with `rb-cli get` |
| EFS CD-ROM | in only | none | `rb-cli optical new sgi-efs --from-dir`; guest: `mount -t efs -o ro /dev/dsk/dks0d4s7 /mnt` |
| Built-in NFS | both | NVRAM `eaddr` + an IP the NAT routes | fine for big trees; needs one-time PROM setup (see ci/iris-irix53.toml comments) |
| Scratch volume | both | none | raw sectors + guest `dd`; `iris-ci put/get`; no filesystem — last resort |

## Driving the guest: lessons baked into iris-build.sh

- **All status comes back through files or sentinels, not prompts.** The
  serial console is hostile to parsing: hostnames differ, root's shell differs
  (csh on stock IRIX, bash on many dev disks), and bash's ANSI color resets
  break `iris-ci run`'s `\nIRIS-CI-RC=` exit-code marker. Every guest step ends
  in `echo TOKEN-'OK'` — quoted when *typed* so the command echo can't match,
  contiguous when *printed* — and the host waits for the printed form.
- **Guest command lines are csh-AND-sh clean**: `;`, `&&`, `||`, `( )` only —
  no `$?`, no `{ }`, no redirects.
- **The PROM is scriptable** over the same serial socket: menu → `5` (command
  monitor) → `boot -f dksc(0,1,8)sash` → `boot -f dksc(0,1,0)unix initstate=s`
  is the whole single-user dance. (The PROM itself cannot read EFS — always go
  through sash.)
- **`iris-ci` waits cap at 300 s per call** (client-side socket timeout), so
  long operations loop shorter `serial-wait`s.
- **The control socket path must be short** (Unix `SUN_LEN` ≈ 104 bytes) —
  it lives in `/tmp`, not in the work dir.

## Adapting this to your own IRIX project

1. Point `--image` at your own installed dev disk (`.chd`).
2. In your copy of `iris-build.sh`, change the **staging list** (which files
   land on the work disk) and **`BUILD_CMD`** (what to run inside the guest).
   Everything else — boot, mount, sentinel, extraction — is project-agnostic.
3. If your image splits `/usr` onto another partition, add `mount /usr` after
   the single-user login (5.3 flavor only).
4. In CI, host the image where the runner can fetch it (private release asset,
   object storage — it's your licensed install, keep it private), or use a
   self-hosted runner that already has it on disk.

## One code path, three front doors

Every operation with any logic in it is a script under `scripts/`, and the
GitHub Actions workflow, the self-hosted dispatch, and
`scripts/release-local.sh` all call the **same scripts** — the YAML step
bodies are one-liners, so the paths cannot drift:

| Step | Script | Reads (flag > env > `ci/local.conf`) |
|---|---|---|
| get the emulator | `fetch-iris.sh` | `IRIS_RELEASE_REPO`/`IRIS_TAG` |
| get rb-cli | `ensure-rbcli.sh` | `RB_CLI`, PATH, else downloads |
| flavor on/off | `fetch-image.sh --enabled` | `BUILD_O32`/`BUILD_N32` |
| get the boot disk | `fetch-image.sh` | `IRIX53_IMAGE`/`IRIX65_IMAGE`, else `IRIX53_DISK_URL`/`IRIX65_DISK_URL` |
| preflight | `fetch-image.sh --check-only` | same |
| build | `iris-build.sh` | — |
| package | `package-dist.sh` → `package.sh` | adapts to the binaries present |
| publish | `publish-release.sh` | `gh` auth / `GH_TOKEN` |

Shared resolution (the flavor↔variable mapping, the conf parser, the enable
switches) lives in `scripts/ci-lib.sh`, sourced by all of them.

Secrets are no obstacle: in Actions they are injected as environment
variables (`IRIX53_DISK_URL` etc.), and locally you export the same variables
(or skip them entirely by using local paths). The only YAML-resident pieces
are the things only Actions can do — job topology and the flavor matrix,
runner selection, secret/input→env mapping, `actions/cache` and artifact
transport between jobs, and apt packages on the runner. None of it is logic.

## The release workflow (this repo's implementation)

`.github/workflows/release.yml` is the reference implementation: a matrixed
`build-native` job (o32 → 5.3 guest, n32 → 6.5 guest) whose steps are the
script calls above; a `package` job (`package-dist.sh`); a `release` job
(`publish-release.sh`).

Two ways to supply the images:

- **Hosted runners** (default): set the `IRIX53_DISK_URL` / `IRIX65_DISK_URL`
  secrets to privately hosted `.chd` (or `.zip`-of-`.chd`) URLs. Downloads are
  cached under a key derived from the URL hash.
- **Self-hosted runner**: dispatch with `runner_label` pointing at your runner
  and `irix53_image` / `irix65_image` set to absolute `.chd` paths on that
  machine — nothing is downloaded and the images never leave your infra.

The emulator itself comes prebuilt from the iris releases, **tracking `latest`
by default**. Pin it with the `IRIS_TAG` repo variable (bump deliberately), or
per-run with the `iris_tag` dispatch input; `IRIS_RELEASE_REPO` overrides
which repo the releases come from (default `danifunker/iris`).

**Enabling/disabling a flavor:** only have one of the two images? The same
switch exists at every front door — `BUILD_O32=0`/`BUILD_N32=0` in
`ci/local.conf` (or the environment) locally, the `build_o32`/`build_n32`
dispatch inputs or `BUILD_O32`/`BUILD_N32` repo variables in Actions (the
preflight job computes the build matrix from them), and `--skip-o32`/
`--skip-n32` on `release-local.sh`. Packaging adapts: with no o32 pair the
images carry the n32 binaries instead, with a loud 6.x-only note.

### Third option: build AND release from your own machine

No hosted secrets, no runner to register — `scripts/release-local.sh` is the
local twin of the whole pipeline: it preflights (clean tree, HEAD pushed, `gh`
authenticated), runs both native builds against the disks in
`ci/local.conf`, packages, and publishes the GitHub release with
`gh release create`, uploading the same artifact set the Actions run would.

```sh
scripts/release-local.sh                    # date-stamped version, publish
scripts/release-local.sh --version 1.0      # explicit version -> tag v1.0
scripts/release-local.sh --dry-run          # build + package, print the gh
                                            # command instead of publishing
scripts/release-local.sh --draft            # publish as a draft release
scripts/release-local.sh --skip-n32         # only a 5.3 image available
```

It refuses to run on a dirty tree (the binaries would stamp `<rev>-dirty`;
`--allow-dirty` overrides) and on a HEAD the remote doesn't have — a release
tag must point at a pushed commit.

### Which hosts can run the emulator step?

`fetch-iris.sh` maps every target the IRIS release pipeline publishes CLI
builds for — linux/macos/windows × x64/arm64 (+ linux-riscv64) — and its
extraction is layout-tolerant, so it keeps working as the release packaging
evolves. **Since iris release v2026-07-28-20-04 every CLI archive bundles
both `iris` and `iris-ci`**, so the prebuilt download is CI-complete on every
target:

| Host | prebuilt pair | iris-build.sh |
|---|---|---|
| linux x64 / arm64 | ✅ | ✅ (the CI default) |
| linux riscv64 | ✅ | should work (untested host) |
| macos x64 / arm64 | ✅ | ✅ (full build validated on arm64 with the prebuilt pair) |
| windows x64 / arm64 | ✅ | untested (needs the TCP control socket) |

Pinning `--tag` at a release older than v2026-07-28 brings back the old
gap (iris-ci was linux-x64/arm64-only there); `fetch-iris.sh` fails with the
exact workaround if so.

## Timings (Apple M-series host, JIT-enabled iris)

| Step | 5.3 (single-user) | 6.5.22 (multiuser) |
|---|---|---|
| Boot to shell | ~1 min | ~3–5 min |
| make (CLI + Motif GUI) | ~1–2 min | ~1–2 min |
| Whole script, fresh overlay | ~4 min | ~6–8 min |

GitHub-hosted runners (no JIT warm cache, slower cores) should budget roughly
2–3× that; both flavors still fit comfortably in a normal job.
