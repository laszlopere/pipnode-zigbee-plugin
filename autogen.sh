#!/bin/sh
# Regenerate the autotools build files (configure, Makefile.in, ...).
# Run once after a fresh clone, then ./configure && make.

set -e

echo "Running autoreconf..."
autoreconf --install --force --verbose

echo ""
echo "Now run ./configure && make"
