#!/bin/sh
set -e
g++ -std=c++17 -Wall -Wextra -O2 -I../sketch -o test_press test_press.cpp ../sketch/Press.cpp
./test_press
