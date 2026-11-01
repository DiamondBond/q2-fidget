#!/bin/sh
# The Q2's binary and the App Store's zip: dist/fidget.zip holds .fidget/q2app and .fidget/q2-fidget.
# Needs clang and ld.lld (no MIPS sysroot: the program makes its own syscalls).
set -e
cd "$(dirname "$0")"
rm -rf dist && mkdir -p dist/.fidget
clang --target=mipsel-linux-gnu -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008 \
    -mno-abicalls -fno-pic -G0 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-math-errno -O2 -Wall -Wextra -Werror \
    -c fidget.c -o dist/fidget.o
ld.lld -m elf32ltsmip -static -e __start --gc-sections -s -z noexecstack dist/fidget.o -o dist/.fidget/q2-fidget
cp q2app dist/.fidget/
(cd dist && zip -qr fidget.zip .fidget)
echo dist/fidget.zip
