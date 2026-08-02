#!/bin/sh
# Desktop tests. No board, no Arduino toolchain — these are the two files
# deliberately kept free of Arduino dependencies.
set -e
g++ -std=c++17 -Wall -Wextra -O2 -o test_press test_press.cpp Press.cpp
./test_press
