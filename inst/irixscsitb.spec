product irixscsitb
    id "irixscsitb - SCSI toolbox for BlueSCSI/ZuluSCSI"
    image sw
        id "irixscsitb Software"
        version @VERSION@
        subsys o32 default
            id "irixscsitb CLI + GUI (o32, runs on IRIX 5.3-6.5)"
            replaces self
            exp irixscsitb.sw.o32
        endsubsys
        subsys n32
            id "irixscsitb CLI + GUI (n32, IRIX 6.x only, faster)"
            replaces self
            exp irixscsitb.sw.n32
        endsubsys
    endimage
endproduct
