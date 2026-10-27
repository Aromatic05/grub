#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="$ROOT_DIR/PKG"
ARTIFACT="$ROOT_DIR/grub2-latest.tar.gz"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
RUN_BOOTSTRAP=1

usage() {
  cat <<'USAGE'
Usage: ./release-local.sh [options]

Options:
  --skip-bootstrap  Skip running ./bootstrap
  -h, --help        Show this help

Env:
  JOBS=<n>          Parallel jobs for make (default: CPU count)
USAGE
}

log() {
  printf '[release-local] %s\n' "$*"
}

require_cmd() {
  local missing=()
  local c
  for c in "$@"; do
    if ! command -v "$c" >/dev/null 2>&1; then
      missing+=("$c")
    fi
  done
  if [ "${#missing[@]}" -ne 0 ]; then
    printf 'Missing commands: %s\n' "${missing[*]}" >&2
    exit 1
  fi
}

prepare_build_dir() {
  local build_dir="$1"
  mkdir -p "$build_dir"
  if [ -f "$build_dir/Makefile" ]; then
    make -C "$build_dir" distclean >/dev/null 2>&1 || make -C "$build_dir" clean >/dev/null 2>&1 || true
  fi
}

build_target() {
  local name="$1"
  local build_dir="$2"
  shift 2

  log "configure ${name}"
  prepare_build_dir "$build_dir"
  (
    cd "$build_dir"
    "$ROOT_DIR/configure" "$@"
  )

  log "make ${name} (-j${JOBS})"
  make -C "$build_dir" -j"$JOBS"

  log "install ${name} -> ${PKG_DIR}"
  make -C "$build_dir" install DESTDIR="$PKG_DIR"

  log "clean ${name}"
  make -C "$build_dir" clean
}

build_target_platform_only() {
  local name="$1"
  local build_dir="$2"
  local platform_dir="$3"
  shift 3

  log "configure ${name}"
  prepare_build_dir "$build_dir"
  (
    cd "$build_dir"
    "$ROOT_DIR/configure" "$@"
  )

  # Use full make to match manual build behavior.
  log "make ${name} (-j${JOBS})"
  make -C "$build_dir" -j"$JOBS"

  log "collect platform payloads for ${name} -> ${platform_dir}"
  local src="$build_dir/grub-core"
  local dst="$PKG_DIR/lib/grub/$platform_dir"
  mkdir -p "$dst"

  shopt -s nullglob
  local f
  for f in \
    "$src/"*.mod \
    "$src/"*.lst \
    "$src/kernel.img" \
    "$src/kernel.exec" \
    "$src/gdb_helper.py" \
    "$src/gdb_grub" \
    "$src/modinfo.sh"
  do
    [ -f "$f" ] || continue
    cp -f "$f" "$dst/"
  done
  shopt -u nullglob

  log "clean ${name}"
  make -C "$build_dir" clean
}

makepkg_layout() {
  local grub_lib="$PKG_DIR/lib/grub"
  local font_dir="$grub_lib/fonts"
  local build_x86_64_efi="$ROOT_DIR/build-x86_64-efi"
  local mkfont_bin="$grub_lib/grub-mkfont"

  log "prepare makepkg layout"
  mkdir -p "$font_dir"

  if [ -d "$ROOT_DIR/makepkg" ]; then
    shopt -s nullglob
    local makepkg_files=( "$ROOT_DIR/makepkg/"* )
    shopt -u nullglob
    if [ "${#makepkg_files[@]}" -gt 0 ]; then
      cp -r "${makepkg_files[@]}" "$grub_lib/"
    fi
  else
    log "warning: $ROOT_DIR/makepkg not found, skip copying makepkg assets"
  fi

  shopt -s nullglob
  local pf2_files=( "$PKG_DIR/share/grub/"*.pf2 )
  shopt -u nullglob
  if [ "${#pf2_files[@]}" -gt 0 ]; then
    cp -f "${pf2_files[@]}" "$font_dir/"
  fi

  # Only copy Linux binaries from build-x86_64-efi.
  if [ -d "$build_x86_64_efi" ]; then
    local f
    shopt -s nullglob
    for f in "$build_x86_64_efi"/grub-*; do
      [ -f "$f" ] || continue
      [ -x "$f" ] || continue
      if file -b "$f" | grep -q 'ELF'; then
        cp -f "$f" "$grub_lib/"
      fi
    done
    shopt -u nullglob
  else
    log "warning: $build_x86_64_efi not found, skip binary copy"
  fi

  # If no .pf2 fonts were installed, try generating one with grub-mkfont.
  shopt -s nullglob
  local existing_pf2=( "$font_dir/"*.pf2 )
  shopt -u nullglob
  if [ "${#existing_pf2[@]}" -eq 0 ] && [ -x "$mkfont_bin" ]; then
    local src_font=""
    local cand
    for cand in \
      /usr/share/fonts/truetype/unifont/unifont.ttf \
      /usr/share/fonts/opentype/unifont/unifont.otf \
      /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
    do
      if [ -f "$cand" ]; then
        src_font="$cand"
        break
      fi
    done
    if [ -n "$src_font" ]; then
      log "generate fallback font unicode.pf2 from $src_font"
      "$mkfont_bin" -s 16 -o "$font_dir/unicode.pf2" "$src_font"
    else
      log "warning: no source TTF/OTF found; font generation skipped"
    fi
  fi

  log "compile locale .po -> .mo"
  shopt -s nullglob
  local po
  for po in "$grub_lib/locale/"*.po; do
    msgfmt "$po" -o "${po%.po}.mo"
  done
  shopt -u nullglob

  rm -rf "$grub_lib/i386-none"
  rm -f "$grub_lib/locale/"'*.po'
  rm -f "$grub_lib/"*/*.module
  rm -f "$grub_lib/"*/*.image
  rm -f "$grub_lib/"*/*.exec
  rm -f "$grub_lib/"*/config.h
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --skip-bootstrap)
      RUN_BOOTSTRAP=0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown option: %s\n\n' "$1" >&2
      usage
      exit 1
      ;;
  esac
  shift
done

require_cmd make tar msgfmt
require_cmd iasl lzop autopoint bison flex gettext

# Cross compilers needed by enabled targets in this script.
require_cmd aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy
require_cmd arm-linux-gnueabihf-gcc arm-linux-gnueabihf-objcopy

if [ "$RUN_BOOTSTRAP" -eq 1 ]; then
  log "bootstrap"
  "$ROOT_DIR/bootstrap"
fi

if [ ! -x "$ROOT_DIR/configure" ]; then
  printf 'configure is missing or not executable: %s\n' "$ROOT_DIR/configure" >&2
  exit 1
fi

log "prepare package directory"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"

# Reuse fixed out-of-tree build directories.
# Non-x86_64 targets: install only platform payloads (no user-space tools).
build_target_platform_only "arm64-efi" "$ROOT_DIR/build-arm64-efi" \
  "arm64-efi" \
  --prefix=/ --target=aarch64-linux-gnu --with-platform=efi

build_target_platform_only "arm-efi" "$ROOT_DIR/build-arm-efi" \
  "arm-efi" \
  --prefix=/ --target=arm-linux-gnueabihf --with-platform=efi

# x86_64-efi: full build/install (includes user-space tools).
build_target "x86_64-efi" "$ROOT_DIR/build-x86_64-efi" \
  --prefix=/ --target=x86_64 --with-platform=efi --disable-werror

build_target_platform_only "i386-efi" "$ROOT_DIR/build-i386-efi" \
  "i386-efi" \
  --prefix=/ --target=i386 --with-platform=efi --disable-werror

build_target_platform_only "i386-pc" "$ROOT_DIR/build-i386-pc" \
  "i386-pc" \
  --prefix=/ --target=i386 --with-platform=pc --disable-werror

build_target_platform_only "i386-multiboot" "$ROOT_DIR/build-i386-multiboot" \
  "i386-multiboot" \
  --prefix=/ --target=i386 --with-platform=multiboot --disable-werror

makepkg_layout

log "create artifact $ARTIFACT"
rm -f "$ARTIFACT"
(
  cd "$PKG_DIR/lib"
  tar -zcf grub.tar.gz grub
)
mv "$PKG_DIR/lib/grub.tar.gz" "$ARTIFACT"

log "done"
