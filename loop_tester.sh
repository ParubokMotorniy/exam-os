#!/bin/sh

# gdb --args ./build/loop 0 ./config_local.cfg
./build/loop 0 ./config_local.cfg &
./build/loop 2 ./config_local.cfg &
./build/loop 1 ./config_local.cfg &
