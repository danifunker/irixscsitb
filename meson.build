project('irixscsitb', 'c')

srcs = [ 'irixscsitb.c' ]

if build_machine.kernel() == 'linux'
    srcs += 'linux.c'
endif

executable('irixscsitb', srcs, install : true)
