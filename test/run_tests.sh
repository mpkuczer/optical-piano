#!/bin/sh
# Every pure layer, on the desktop. No board needed.
#
# The paths said ../sketch, which has not been the layout for some time — the
# sources sit one directory up. Everything here has been failing at the first
# line since that rename, which is also why test_gesture.cpp and host/ are
# still listed below: they are named by a script nobody could run. They do not
# exist in the repo. Restore them or delete the lines; leaving them commented
# out at least makes the gap visible instead of fatal.
#
# test_modmatrix.cpp now does exist — it arrived with CURVE_DETENT, whose whole
# purpose is a promise about specific cent values.
set -e
CXX="g++ -std=c++17 -Wall -Wextra -O2 -I.."
#
# ORDER IS BOTTOM-UP, and it has to be while `set -e` is in force: pipeline
# currently fails one case (the crosstalk fixture predates contOn rising to
# 0.150, so the bug it reproduces no longer reproduces), and anything listed
# after it never runs at all.
echo "== calibration ==" && $CXX -o t_cal test_calibration.cpp ../Calibration.cpp && ./t_cal
echo "== modmatrix =="   && $CXX -o t_mod test_modmatrix.cpp ../ModMatrix.cpp && ./t_mod
echo "== options =="     && $CXX -o t_opt test_options.cpp  ../Options.cpp && ./t_opt
echo "== pipeline =="    && $CXX -o t_pip test_pipeline.cpp ../Calibration.cpp ../Gesture.cpp && ./t_pip
# echo "== gesture =="     && $CXX -o t_ges test_gesture.cpp   ../Gesture.cpp && ./t_ges
# echo "== whole sketch ==" && (cd host && sh run_host.sh)
