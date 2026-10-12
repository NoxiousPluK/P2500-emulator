#!/bin/sh
# Removes build artefacts: build/ (every object and both toolchains'
# libp2500.a), the binaries - native and Windows, whichever are present -
# the built demo disk and its .COM files, Python's __pycache__ caches, and
# publish/ (publish.sh's output).
# Does not touch disks/, roms/, tests/fixtures/, or anything else checked in.
set -eu
cd "$(dirname "$0")"

make clean
rm -rf publish

find . -type d -name '__pycache__' -not -path './.git/*' -exec rm -rf {} +

echo "Clean."
