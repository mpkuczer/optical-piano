#!/bin/sh
# Every pure layer, on the desktop. No board, no Arduino toolchain.
set -e
CXX="g++ -std=c++17 -Wall -Wextra -O2 -I../sketch"
echo "== calibration ==" && $CXX -o t_cal test_calibration.cpp ../sketch/Calibration.cpp && ./t_cal
echo "== gesture ==="    && $CXX -o t_ges test_gesture.cpp     ../sketch/Gesture.cpp     && ./t_ges
echo "== modmatrix =="   && $CXX -o t_mod test_modmatrix.cpp   ../sketch/ModMatrix.cpp   && ./t_mod
