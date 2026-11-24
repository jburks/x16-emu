#!/bin/sh
# Rebuilds the fixture: a raw binary loaded at $0400, its debug info and its label file.
set -e
cd "$(dirname "$0")"
ca65 -g calls.s -o calls.o
ld65 -C calls.cfg -o calls.bin --dbgfile calls.dbg -Ln calls.sym calls.o
rm -f calls.o
