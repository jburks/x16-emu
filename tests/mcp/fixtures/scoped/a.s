; Test fixture for test_dbgsym.py (module A). Rebuild with ./build.sh (needs cc65); the outputs are committed.
        .export procA, shared_const
        .import procB

COUNT        = 3         ; a plain constant: neither a label nor exported, so not loaded
shared_const = $1234     ; an exported constant that module B imports: loaded

        .segment "CODE"
start:  jsr procA
        jsr procB
        jsr outer
done:   jmp done         ; "done" is also a top-level label in module B

.proc procA
        ldx #COUNT
loop:   dex              ; procA::loop
        bne loop
        rts
.endproc

.proc outer
        jsr inner
        rts
.proc inner
        ldy #2
loop:   dey              ; outer::inner::loop
        bne loop
        rts
.endproc
.endproc
