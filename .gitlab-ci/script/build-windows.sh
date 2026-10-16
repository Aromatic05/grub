#!/bin/bash

set -xeu -o pipefail

# Set the number of parallel jobs for make, defaulting to 1 if it cannot be determined
CPU_COUNT=1

if command -v nproc >/dev/null 2>&1
then
    CPU_COUNT=$(nproc 2>/dev/null)
fi

H="${TARGET}"
T="${TARGET}"
P="${PLATFORM}"

if [ "$P" = efi ]; then
    ./configure --build=x86_64-linux-gnu --host="$H" --target="$T" --with-platform="$P" --enable-stack-protector
else
    ./configure --build=x86_64-linux-gnu --host="$H" --target="$T" --with-platform="$P"
fi

make -j "$CPU_COUNT"
make -j "$CPU_COUNT" windowszip
