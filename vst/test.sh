#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of the plugin (host_test.cpp), the convolver against a plain
# convolution (test_conv.cpp), then a CPU benchmark (-O2): run after build.sh (needs
# build/params.h, build/popup.h). Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/b -w /b gcc:12 bash -euc '
  INC="-Ibuild -I. -Igx -Igx/dsp -Igx/faust -Igx/tube_tables -Igx/zita-resampler"
  mkdir -p build/x86
  build() {   # $1 = output .so, $2.. = flags
    out=$1; shift
    objs=""
    for f in $(cat sources.txt); do
      o=build/x86/$(basename "$f").$(basename "$out").o
      g++ "$@" -std=c++17 -fPIC $INC -c "$f" -o "$o"
      objs="$objs $o"
    done
    g++ "$@" -shared -o "$out" $objs -lpthread
  }
  SAN="-fsanitize=address,undefined"
  build build/x86/gxamp-x86.so -O1 -g $SAN
  g++ -O1 -g $SAN -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/gxamp-x86.so
  g++ -O2 -std=c++17 $INC -o build/x86/test_conv test_conv.cpp $(grep -v gxamp_vst.cpp sources.txt) -lpthread
  ./build/x86/test_conv
  build build/x86/gxamp-o2.so -O3 -ffast-math -fvisibility=hidden
  g++ -O2 -std=c++17 -o build/x86/bench host_test.cpp -ldl
  ./build/x86/bench ./build/x86/gxamp-o2.so bench
'
