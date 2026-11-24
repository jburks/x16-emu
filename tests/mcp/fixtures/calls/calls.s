; Test fixture for test_calls.py: call tracking (profile_start calls: true). Rebuild with ./build.sh (needs
; cc65); the outputs are committed. Loaded raw at $0400; run from start to done.

        .segment "CODE"
start:  jsr outer        ; nested calls: outer calls leaf twice
        ldx #3
        jsr rec          ; recursion: rec runs 3 times
        jsr mid          ; mid calls dropper, which drops mid's return address
        jsr dispatch     ; an RTS jump table: leaf is reached by RTS, not called
        jsr tailer       ; a tail call: tailer jumps to leaf
        jsr waiter       ; a long loop with interrupts enabled
done:   jmp done

.proc leaf
        lda #0           ; 2 cycles
        rts              ; 6: leaf costs 8 cycles a call
.endproc

.proc outer
        jsr leaf
        jsr leaf
        rts
.endproc

.proc rec
        dex
        beq out
        jsr rec
out:    rts
.endproc

.proc mid
        jsr dropper
        nop              ; never reached: dropper returns to mid's caller
        rts
.endproc

.proc dropper
        pla              ; drop mid's return address...
        pla
        rts              ; ...and return to start
.endproc

.proc dispatch
        lda #>(leaf - 1)
        pha
        lda #<(leaf - 1)
        pha
        rts              ; "returns" into leaf, whose rts returns to start
.endproc

.proc tailer
        ldy #1
        jmp leaf         ; leaf replaces tailer: its rts returns to start
.endproc

.proc waiter
        cli
        ldy #0
outer_loop:
        ldx #0
inner_loop:
        dex
        bne inner_loop
        dey
        bne outer_loop   ; about 330,000 cycles: several frames, so interrupts come in
        sei
        rts
.endproc

; A second program (run from nest_start to nest_done): a jump into a nested .proc is not a tail call.
nest_start:
        jsr nestjump
nest_done:
        jmp nest_done

.proc nestjump
        ldy #2
        jmp part         ; into the nested .proc: still nestjump
.proc part
        dey
        bne part         ; a branch to the nested scope's own start: a loop
        rts
.endproc
.endproc
