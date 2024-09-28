#!/bin/sh

./configure \
  --with-version=5.1.0-0-g0 \
  --with-lg-quantum=3 \
  --with-jemalloc-prefix=je_
make lib/libjemalloc.a
