project('irixscsitb', 'c')

# CLI front end + protocol core. The Motif GUI (gui_motif.c) is IRIX-only and
# built from the Makefile; there is no meson target for it.
srcs = [ 'irixscsitb.c', 'toolbox.c' ]

if build_machine.kernel() == 'linux'
    srcs += 'linux.c'
    # The Makefile passes this too; without it the usage text and the
    # "is the sg module loaded?" hints compile out.
    add_project_arguments('-DOS_LINUX', language : 'c')
endif

executable('irixscsitb', srcs, install : true)
