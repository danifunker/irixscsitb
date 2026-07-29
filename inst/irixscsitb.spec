product irixscsitb
    id "irixscsitb - SCSI toolbox for BlueSCSI/ZuluSCSI"
    image sw
        id "irixscsitb Software"
        version @VERSION@
        subsys @SUBSYS@ default
            id "irixscsitb CLI + GUI (@ABI_DESC@)"
            replaces self
            exp irixscsitb.sw.@SUBSYS@
        endsubsys
    endimage
endproduct
