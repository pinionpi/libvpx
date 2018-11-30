#!/bin/sh

emconfigure ./configure --target=generic-gnu \
  --disable-multithread \
  --disable-runtime-cpu-detect \
  --disable-mmx \
  --disable-sse \
  --disable-sse2 \
  --disable-sse3 \
  --disable-ssse3 \
  --disable-optimizations \
  --disable-unit-tests \
  --disable-install-docs \
  --disable-examples

emmake make

emcc -v -s ALLOW_MEMORY_GROWTH=1 libvpx.a -o libvpx.js

ls -l libvpx.*
