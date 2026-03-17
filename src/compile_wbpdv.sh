#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++23
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat -Wformat-security
  -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS
  -O3 -pipe
  -fstack-protector-strong -fstack-clash-protection -fPIE
  -s -flto=auto -DNDEBUG
  $(pkg-config --cflags libwebp 2>/dev/null || echo "-I/tmp/webp_dev/usr/include")
)

LDFLAGS=(
  -pie -Wl,-z,relro,-z,now,-z,noexecstack
  $(pkg-config --libs-only-L libwebp 2>/dev/null || echo "-L/tmp/webp_dev/usr/lib/x86_64-linux-gnu")
)

SOURCES=( *.cpp )
LIBS=( -lsodium -lz -lwebp )

echo "Compiling wbpdv..."
"$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${LDFLAGS[@]}" "${LIBS[@]}" -o wbpdv
echo "Compilation successful. Executable 'wbpdv' created."
