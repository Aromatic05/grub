#!/bin/bash

set -xeu -o pipefail

# Set the number of parallel jobs for make, defaulting to 1 if it cannot be determined
CPU_COUNT=1

if command -v nproc >/dev/null 2>&1
then
    CPU_COUNT=$(nproc 2>/dev/null)
fi

PATH="$CROSS_C/ia64-linux/bin:$CROSS_C/loongarch64-linux/bin:$CROSS_C/riscv32-linux/bin:$PATH"
i="${TARGET}"
j="${PLATFORM}"

if [ "$j" = efi ] && [ "$i" != ia64-linux ] && [ "$i" != loongarch64-linux ] && [ "$i" != riscv64-linux-gnu ]; then
  ./configure --target="$i" --with-platform="$j" --enable-stack-protector --enable-grub-mkfont --prefix="$(pwd)/grub-dist"
else
  ./configure --target="$i" --with-platform="$j" --enable-grub-mkfont --prefix="$(pwd)/grub-dist"
fi

if [ -n "${COVERITY_SCAN:-}" ]; then
    cov-build --dir "${COVERITY_DIR}" make --quiet -j"${CPU_COUNT}"
else
    make --quiet -j"${CPU_COUNT}" install
    make --quiet html
    make --quiet pdf
fi
