#! /usr/bin/env bash

# Group: Graham Fisher, Tommy Gallagher, Jason Brown

curl http://digip.org/jansson/releases/jansson-2.13.tar.bz2 -O
bunzip2 -c jansson-2.13.tar.bz2 | tar xf -
cd jansson-2.13
./configure --prefix=$(pwd)/..
make
make check
make install

