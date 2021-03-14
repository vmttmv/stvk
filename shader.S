.section .rodata
.global vssrc
.global vssrc_size
.global fssrc
.global fssrc_size

vssrc:
        .incbin "vs.spv"

vssrc_size:
        .int vssrc_size - vssrc

fssrc:
        .incbin "fs.spv"

fssrc_size:
        .int fssrc_size - fssrc
