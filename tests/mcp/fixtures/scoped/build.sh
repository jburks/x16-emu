#!/bin/sh
# Rebuilds the fixture: a raw binary loaded at $0400, its debug info and its label file.
# d.c is compiled to d.s, which is then deleted, as cl65 does with its temporary assembler file.
set -e
cd "$(dirname "$0")"
cc65 -g -O d.c -o d.s
for m in a b c d; do ca65 -g $m.s -o $m.o; done
ld65 -C scoped.cfg -o scoped.bin --dbgfile scoped.dbg -Ln scoped.sym a.o b.o c.o d.o
rm -f a.o b.o c.o d.o d.s
