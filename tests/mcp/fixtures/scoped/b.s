; Test fixture for test_dbgsym.py (module B).
        .export procB
        .import shared_const

        .segment "CODE"
.proc procB
        lda #<shared_const ; imports the constant, which marks it as exported in the debug info
        ldx #2
loop:   dex              ; procB::loop
        bne loop
        rts
.endproc
done:   rts              ; the other top-level "done"
table:  .byte 1, 2, 3
table_end = *            ; an address constant (has a segment): loaded
