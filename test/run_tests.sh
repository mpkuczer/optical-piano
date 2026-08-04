#!/bin/sh
# Every pure layer, on the desktop. No board needed.
#
# The paths said ../sketch, which has not been the layout for some time — the
# sources sit one directory up. Everything here has been failing at the first
# line since that rename, which is also why test_gesture.cpp, test_modmatrix.cpp
# and host/ are still listed below: they are named by a script nobody could run.
# They do not exist in the repo. Restore them or delete the lines; leaving them
# commented out at least makes the gap visible instead of fatal.
set -e
CXX="g++ -std=c++17 -Wall -Wextra -O2 -I.."
echo "== calibration ==" && $CXX -o t_cal test_calibration.cpp ../Calibration.cpp && ./t_cal
echo "== pipeline =="    && $CXX -o t_pip test_pipeline.cpp ../Calibration.cpp ../Gesture.cpp && ./t_pip
# echo "== gesture =="     && $CXX -o t_ges test_gesture.cpp   ../Gesture.cpp && ./t_ges
# echo "== modmatrix =="   && $CXX -o t_mod test_modmatrix.cpp ../ModMatrix.cpp ../Gesture.cpp && ./t_mod
# echo "== whole sketch ==" && (cd host && sh run_host.sh)
