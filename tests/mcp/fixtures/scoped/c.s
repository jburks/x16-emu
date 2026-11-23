; Test fixture for test_dbgsym.py (module C): code from macros and .repeat, for source positions.
        .export macros

.macro  ADD2 v           ; an inner macro
        clc
        adc #v           ; the body line reported for the inner macro's adc
.endmacro

.macro  ADDBOTH v
        ADD2 v           ; nested: the outer macro uses the inner one
        ADD2 v+1
.endmacro

        .segment "CODE"
.proc macros
        lda #0
        ADDBOTH 1        ; call site of the nested macros
        ADD2 5           ; call site of a single macro
        .repeat 3
        inx              ; a .repeat body line
        .endrepeat
        rts
.endproc
