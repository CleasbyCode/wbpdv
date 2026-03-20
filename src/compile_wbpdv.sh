#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-g++}"

WEBP_CFLAGS_STR="$(pkg-config --cflags libwebp 2>/dev/null || echo "-I/tmp/webp_dev/usr/include")"
WEBP_LIBDIR_STR="$(pkg-config --libs-only-L libwebp 2>/dev/null || echo "-L/tmp/webp_dev/usr/lib/x86_64-linux-gnu")"

WEBP_CFLAGS=()
WEBP_LDFLAGS=()
if [[ -n "$WEBP_CFLAGS_STR" ]]; then
  read -r -a WEBP_CFLAGS <<< "$WEBP_CFLAGS_STR"
fi
if [[ -n "$WEBP_LIBDIR_STR" ]]; then
  read -r -a WEBP_LDFLAGS <<< "$WEBP_LIBDIR_STR"
fi

CXXFLAGS=(
  -std=c++23
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat -Wformat-security
  -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS
  -O3 -pipe
  -fstack-protector-strong -fstack-clash-protection -fPIE
  -s -flto=auto -DNDEBUG
)
if [[ ${#WEBP_CFLAGS[@]} -gt 0 ]]; then
  CXXFLAGS+=( "${WEBP_CFLAGS[@]}" )
fi

LDFLAGS=(
  -pie -Wl,-z,relro,-z,now,-z,noexecstack
)
if [[ ${#WEBP_LDFLAGS[@]} -gt 0 ]]; then
  LDFLAGS+=( "${WEBP_LDFLAGS[@]}" )
fi

SOURCES=( *.cpp )

LIBS=( -lsodium -lz -lwebp )

echo "Compiling wbpdv..."
"$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${LDFLAGS[@]}" "${LIBS[@]}" -o wbpdv
echo "Compilation successful. Executable 'wbpdv' created."
