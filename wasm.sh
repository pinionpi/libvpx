#!/bin/sh

emconfigure ./configure --target=generic-gnu \
  --disable-multithread \
  --disable-runtime-cpu-detect \
  --disable-unit-tests \
  --disable-install-docs \
  --disable-examples \
  --enable-realtime-only

emmake make

# Replace -O2 with --profiling --profiling-funcs -g4 and use Chrome's profiler.
emcc -v \
  -O2 \
  -s ALLOW_MEMORY_GROWTH=1 \
  libvpx.a -o libvpx.js

ls -l --block-size K libvpx.*
