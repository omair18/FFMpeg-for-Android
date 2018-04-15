#!/bin/bash
SYSROOT=$NDK/platforms/android-14/arch-x86/
TOOLCHAIN=$NDK/toolchains/x86-4.9/prebuilt/darwin-x86_64
rm -f $(pwd)/compat/strtod.o
function build_one
{
./configure --prefix=$PREFIX $COMMON $CONFIGURATION --cross-prefix=$TOOLCHAIN/bin/i686-linux-android- --target-os=linux --arch=x86 --disable-asm --sysroot=$SYSROOT --extra-cflags="-O3 -Wall -pipe -DANDROID -DNDEBUG  -march=atom -msse3 -ffast-math -mfpmath=sse $ADDI_CFLAGS" --extra-ldflags="-lm -lz -Wl,--no-undefined -Wl,-z,noexecstack $ADDI_LDFLAGS"

make clean
make -j2
make install
}

export CPU=x86
PREFIX=$(pwd)/android/$CPU 
build_one
cp Android.mk $PREFIX/Android.mk
cd $PROJECT_JNI
export ABI=$CPU
export PLATFORM="android-14"
ndk-build
cp -r "$PROJECT_LIBS/$CPU" "$PROJECT_LIBS/../out" 
cd $DIR
