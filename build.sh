#!/bin/bash
# Build mountain screensaver for SymbOS using scc

SCC="${SCC:-../scc/bin/cc}"

"$SCC" mountain.c mountain_msx.s \
    -N "Mountain" \
    -o mountain.sav \
    -h 512

python3 add_preview.py
