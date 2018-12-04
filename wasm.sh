#!/bin/sh

emconfigure ./configure --target=generic-gnu \
  --disable-runtime-cpu-detect \
  --disable-unit-tests \
  --disable-install-docs \
  --disable-examples \
  --enable-realtime-only \
  --enable-pic \
  --enable-multi-res-encoding \
  --enable-temporal-denoising \

emmake make

emcc -v \
  -O3 --profiling --profiling-funcs -g4 \
  -s ALLOW_MEMORY_GROWTH=1 \
  libvpx.a -o libvpx.js

ls -l --block-size K *.{wasm,js}
