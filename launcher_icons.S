.section .rodata
.balign 4
.global opds_launcher_icon_bmp
.type opds_launcher_icon_bmp, %object
opds_launcher_icon_bmp:
.incbin "Pergamo.bmp"
opds_launcher_icon_bmp_end:
.size opds_launcher_icon_bmp, opds_launcher_icon_bmp_end - opds_launcher_icon_bmp

.balign 4
.global opds_launcher_icon_focused_bmp
.type opds_launcher_icon_focused_bmp, %object
opds_launcher_icon_focused_bmp:
.incbin "Pergamo_f.bmp"
opds_launcher_icon_focused_bmp_end:
.size opds_launcher_icon_focused_bmp, opds_launcher_icon_focused_bmp_end - opds_launcher_icon_focused_bmp

.balign 4
.global opds_launcher_icon_bmp_len
.type opds_launcher_icon_bmp_len, %object
opds_launcher_icon_bmp_len:
.word opds_launcher_icon_bmp_end - opds_launcher_icon_bmp
.size opds_launcher_icon_bmp_len, . - opds_launcher_icon_bmp_len

.balign 4
.global opds_launcher_icon_focused_bmp_len
.type opds_launcher_icon_focused_bmp_len, %object
opds_launcher_icon_focused_bmp_len:
.word opds_launcher_icon_focused_bmp_end - opds_launcher_icon_focused_bmp
.size opds_launcher_icon_focused_bmp_len, . - opds_launcher_icon_focused_bmp_len

.section .note.GNU-stack,"",%progbits
