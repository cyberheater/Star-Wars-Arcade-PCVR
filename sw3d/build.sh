#!/bin/sh
# Build SW3D (the Star Wars C port on SDL2) - macOS or Linux.
#   needs: a C/C++ compiler and SDL2 (macOS: brew install sdl2)
#   output: ./sw3d, which reads the ROM images from ./roms (see README.md)
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
CXX=${CXX:-c++}
OPT="-O2"
SDL_CFLAGS=$(sdl2-config --cflags)
SDL_LIBS=$(sdl2-config --libs)
case "$(uname -s)" in
    Darwin) GL_LIBS="-framework OpenGL"; DEFS="-DGL_SILENCE_DEPRECATION" ;;
    *)      GL_LIBS="-lGL"; DEFS="" ;;
esac

mkdir -p obj/main obj/snd obj/core
jobs=""
run() { "$@" & jobs="$jobs $!"; }
waitall() { for j in $jobs; do wait "$j"; done; jobs=""; }

# the main program (gen/, hand/) and its runtime
for f in src/gen/*.c src/hand/*.c src/sw_rt.c; do
    run $CC -std=c11 $OPT -w -Isrc -c "$f" -o "obj/main/$(basename "$f" .c).o"
done
waitall
# the sound program and its own build of the runtime
for f in src/gen_snd/*.c; do
    run $CC -std=c11 $OPT -w -Isrc -c "$f" -o "obj/snd/$(basename "$f" .c).o"
done
run $CC -std=c11 $OPT -w -DSW_SND_PROGRAM -Isrc -c src/sw_rt.c -o obj/snd/sw_rt.o
# the chip models, the machine, the backend
for f in avg mathbox x2212 riot6532 c012294 fiber_posix sw3d_capture; do
    [ -f "src/$f.c" ] && run $CC -std=c11 $OPT -w -Isrc -c "src/$f.c" -o "obj/core/$f.o"
done
for f in tms5220 hw app port; do
    run $CXX -std=c++17 $OPT -w -Isrc -c "src/$f.cpp" -o "obj/core/$f.o"
done
run $CXX -std=c++17 $OPT -Wall $DEFS $SDL_CFLAGS -Isrc -c src/platform/sdl/plat_sdl.cpp -o obj/core/plat_sdl.o
waitall

$CXX -o sw3d obj/main/*.o obj/snd/*.o obj/core/*.o $SDL_LIBS $GL_LIBS
echo "BUILD OK: $(pwd)/sw3d"
