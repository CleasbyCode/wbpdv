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
  # -march=native tunes and emits instructions for THIS machine's CPU (e.g. AVX2,
  # SSSE3). The resulting binary is NOT redistributable: running it on an
  # older/different CPU can fail with "Illegal instruction" (SIGILL). wbpdv is
  # meant to be built from source on the machine that runs it. If you must ship a
  # portable binary, replace this with a baseline arch (e.g. -march=x86-64-v2) or
  # use -mtune=native for portable codegen; the base64 SIMD tier in base64.cpp is
  # selected at compile time from these flags, so a baseline arch simply falls
  # back to the SSSE3 or scalar kernel.
  -march=native
  -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS
  -O3 -pipe
  -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -fPIE
  -s -flto=auto -DNDEBUG
)
if [[ ${#WEBP_CFLAGS[@]} -gt 0 ]]; then
  CXXFLAGS+=( "${WEBP_CFLAGS[@]}" )
fi

LDFLAGS=(
  -pie -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code
)
if [[ ${#WEBP_LDFLAGS[@]} -gt 0 ]]; then
  LDFLAGS+=( "${WEBP_LDFLAGS[@]}" )
fi

SOURCES=( *.cpp )

LIBS=( -lsodium -lz -ldeflate -lwebp )

echo "Compiling wbpdv..."
"$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${LDFLAGS[@]}" "${LIBS[@]}" -o wbpdv
echo "Compilation successful. Executable 'wbpdv' created."
