#!/bin/sh
set -eu
: "${WATCOM:?Set WATCOM to the Open Watcom installation directory}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source="$here/../source"
out=${1:-"$here/build"}
mkdir -p -- "$out"
out=$(CDPATH= cd -- "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd -- "$work"

PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
INCLUDE="$WATCOM/h"
export WATCOM PATH INCLUDE

cp "$source/TINY88.C" tiny88.c
cp "$source/ENGINE.C" engine.c
cp "$source/ENGINE.H" "$source/KERNELS.ASM" .
wasm -q -0 -ml KERNELS.ASM
wcl -q -0 -ml -os -s -zp1 -bt=dos -k8192 -fe=TINY88.EXE tiny88.c engine.c KERNELS.o
mv -- TINY88.EXE "$out/TINY88.EXE"
