#!/bin/sh
# Rebuilds the fixture: a raw binary loaded at $0400, its debug info and its label file.
set -e
cd "$(dirname "$0")"
ca65 -g a.s -o a.o
ca65 -g b.s -o b.o
ld65 -C scoped.cfg -o scoped.bin --dbgfile scoped.dbg -Ln scoped.sym a.o b.o
rm -f a.o b.o
