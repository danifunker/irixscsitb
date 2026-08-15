# Driving IRIS as an IRIX build agent (`iris` + `iris-ci`)

> **Status: proposed / deferred.** `scripts/fetch-iris.sh` and this note are not
> yet wired into `release.yml` (which still builds IRIS from source). Shipping
> `iris-ci` in the IRIS release archives — the thing `fetch-iris.sh` relies on —
> is likewise staged but disabled upstream for now. Flip both on together later.

How the headless CI build talks to the IRIS emulator to compile irixscsitb
natively inside IRIX. This is the mental model behind `scripts/iris-build.sh`
and the `build-o32-native` job in `.github/workflows/release.yml`.

## Two processes, not one

The build agent is **two** programs from the IRIS release, and the split matters:

| Program   | Role | Uses CHD? | Opens a socket? |
|-----------|------|-----------|-----------------|
| `iris`    | The **emulator**. Attaches the disk image, runs the MIPS CPU, boots IRIX, and exposes a control socket. | **Yes** — `--scsi1 disk.chd` (needs the `chd` feature) | serves it (`--ci`) |
| `iris-ci` | A **thin client**. Connects to `iris`'s socket and sends JSON commands (`boot`, `login`, `run`, `put`/`get`, …). No emulator, no CHD code — clap + serde_json + std sockets. | No | connects to it |

So **`iris` boots the CHD, not `iris-ci`.** `iris-ci boot` does not load a disk
image — it walks the PROM menu of an already-running emulator (start CPU → wait
for the `Option?` menu → send `1` "Start System" → wait for the login prompt) to
reach a shell. The CHD must already be attached to the `iris` process at launch.
`iris-ci` on its own can't build anything; it needs an `iris` running with the
boot disk mounted.

`iris-ci run "…"` returns the guest command's stdout **and propagates its exit
code** (non-zero guest exit → `iris-ci` exits 2), which is why a failing `make`
inside IRIX fails the CI step. That orchestration is exactly what downstream
shouldn't reimplement over the raw socket.

## The flow

```
             ┌──────────── host (Linux CI runner) ────────────┐
             │                                                 │
  iris  --ci --config ci/iris-irix53.toml --nfs-dir SHARE      │  (background)
    │   attaches irix53.chd, boots IRIX, serves /tmp/iris.sock │
    │                          ▲                               │
    │   control socket         │ JSON over the socket          │
    ▼                          │                               │
  iris-ci ping   → wait for the socket to come up              │
  iris-ci boot   → PROM menu → IRIX `console login` prompt     │
  iris-ci login  → root shell                                  │
  iris-ci run "mount GW:/ /mnt && cd … && make && cp bin /mnt" │
             │                                                 │
  the built binary lands on the NFS SHARE on the host ─────────┘
```

`scripts/iris-build.sh` implements exactly this. File transfer is over IRIS's
built-in in-core NFS server (`--nfs-dir`, always compiled in — not a feature
flag), so there's nothing to install and no `dd bs=512` scratch-volume math. The
older `iris-ci put`/`get` scratch-volume path still exists as a no-networking
fallback (see the commented `[scsi.2]` block in `ci/iris-irix53.toml`).

## Getting `iris` + `iris-ci`: download prebuilt, don't build from source

Every Linux `IRIS-cli-*` release archive from the IRIS pipeline now ships **both**
binaries, flat at the archive root:

```
$ tar tzf IRIS-cli-r4400-linux-x64-<ver>.tar.gz
iris
iris-ci
LICENSE
LICENSE-libchdman-rs.txt
```

So CI no longer needs to clone IRIS and `cargo build` the emulator (a Rust
toolchain + clang/libclang for the `chd` feature + a full release build — the
long pole of the o32 job). Use **`scripts/fetch-iris.sh`** instead:

```sh
# Drops iris + iris-ci into iris/target/release/ — the layout
# scripts/iris-build.sh --iris-dir iris already expects.
scripts/fetch-iris.sh --dir iris          # latest danifunker/iris release, host arch
```

The script tries the `r4400` variant first — iris releases since
v2026-08-13-11-14 ship per-emulated-CPU builds (`r4400`/`r5000`), and r4400 is
the Indy both guests boot (an R5000 Indy needs IRIX 6.2+, so it covers 5.3 and
6.5 alike) — then falls back to `lightning` for tags from before the rename.
(Every variant carries `iris-ci` and `chd`.)

### Wiring it into `release.yml`

In `build-o32-native`, **replace** the "Clone IRIS source" + "Build iris +
iris-ci" steps (and their Rust toolchain / clang / rust-cache setup) with:

```yaml
      - name: Fetch prebuilt iris + iris-ci
        env:
          GH_TOKEN: ${{ github.token }}
        run: ./scripts/fetch-iris.sh --dir iris
```

Everything after is unchanged: copy the boot disk to `iris/irix53.chd`,
DHCP-enable it, then run `iris-build.sh --iris-dir iris …`. `fetch-iris.sh`
populates `iris/target/release/{iris,iris-ci}`, which is what `--iris-dir iris`
reads. Building IRIS from source stays available (e.g. to test an unreleased
emulator change) — just keep the old clone+build steps for that path.

## Keep the boot disk disposable

`iris` writes to its boot disk while IRIX runs (and `irix-enable-dhcp.sh` mutates
the `.chd` in place before boot). A run killed mid-boot can corrupt the guest
filesystem — an **uncompressed** CHD is written in place, so damage hits the base
image directly; a **compressed** CHD diverts writes to a sidecar `.diff.chd`,
leaving the base pristine.

- **In CI this is already safe**: each run downloads a fresh boot disk onto an
  ephemeral runner, so in-place mutation is thrown away with the runner.
- **For local iteration**, don't boot your only copy of the golden image. Either
  use a compressed `.chd` (writes go to `.diff.chd`; delete it to reset), or keep
  a pristine master and copy it in per run. Then a timed-out or crashed build
  never poisons the base image.

## Quick reference

```sh
scripts/fetch-iris.sh --dir iris                 # get prebuilt iris + iris-ci
cp path/to/irix53.chd iris/irix53.chd            # boot disk where the toml expects it
RB_CLI=./rb-cli CHDMAN=chdman \
  scripts/irix-enable-dhcp.sh iris/irix53.chd    # one-time: DHCP so NFS has an IP
scripts/iris-build.sh --iris-dir iris --no-package --bin-out irixscsitb-o32
```

Under the hood `iris-build.sh` launches `iris --ci --config ci/iris-irix53.toml
--nfs-dir <share>`, waits on `iris-ci ping`, then `iris-ci boot` / `login` /
`run`, and `iris-ci quit` on cleanup.
