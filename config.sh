#!/bin/sh
#
# Script for building Agens Graph distribution 
#
# This script runs configure with default configure options 
# You can add more options like the install path as follows;
#
# ./config.sh [options]

./configure --with-blocksize=32 --enable-debug $@
