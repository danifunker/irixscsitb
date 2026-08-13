project('irixscsitb', 'c')

# CLI front end + protocol core. The Motif GUI (gui_motif.c) is IRIX-only and
# built from the Makefile; there is no meson target for it.
srcs = [ 'irixscsitb.c', 'toolbox.c', 'wifi.c', 'version.c' ]

# Build identification (see CLAUDE.md "Build identification"). version.c is
# the only file that includes these two generated headers:
#   version.h    WHAT source this is — stamped from git by mkversion.sh, which
#                refuses to downgrade an already-shipped header to "unknown"
#                on a machine without git
#   buildhost.h  WHERE it got compiled — regenerated on every setup
# Both land in the source root (where the Makefile also puts them and where
# version.c's quoted includes look first); both are .gitignore'd.
root = meson.project_source_root()
run_command('sh', root / 'scripts' / 'mkversion.sh', root / 'version.h',
            check : true)
run_command('sh', root / 'scripts' / 'mkbuildhost.sh', root / 'buildhost.h',
            check : true)

if build_machine.kernel() == 'linux'
    srcs += 'linux.c'
    # The Makefile passes this too; without it the usage text and the
    # "is the sg module loaded?" hints compile out.
    add_project_arguments('-DOS_LINUX', language : 'c')
endif

executable('irixscsitb', srcs, install : true)
