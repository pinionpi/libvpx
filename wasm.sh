#!/bin/sh

emconfigure ./configure --target=generic-gnu \
  --disable-multithread \
  --disable-runtime-cpu-detect \
  --disable-unit-tests \
  --disable-install-docs \
  --disable-examples

emmake make

emcc -v \
  --profiling --profiling-funcs -g4 \
  -s ALLOW_MEMORY_GROWTH=1 \
  libvpx.a -o libvpx.js

ls -l --block-size K libvpx.*
