#!/bin/bash
SYSROOT=$NDK/platforms/android-14/arch-mips/
TOOLCHAIN=$NDK/toolchains/mipsel-linux-android-4.9/prebuilt/darwin-x86_64
rm -f $(pwd)/compat/strtod.o
function build_one
{
./configure --prefix=$PREFIX --nm=$TOOLCHAIN/bin/mipsel-linux-android-nm --extra-libs="-lgcc" --enable-cross-compile --cc=$TOOLCHAIN/bin/mipsel-linux-android-gcc $COMMON $CONFIGURATION --cross-prefix=$TOOLCHAIN/bin/mipsel-linux-android- --target-os=linux --disable_asm --arch=mips --sysroot=$SYSROOT --disable-mipsdspr1 --disable-mipsdspr2 --disable-mipsfpu --extra-cflags="-O3 -DANDROID -Dipv6mr_interface=ipv6mr_ifindex -fasm -Wno-psabi -fno-short-enums -fno-strict-aliasing $ADDI_CFLAGS" --extra-ldflags="-Wl,-rpath-link=$SYSROOT/usr/lib -L$SYSROOT/usr/lib -nostdlib -lc -lm -ldl -llog $ADDI_LDFLAGS" --enable-zlib

make clean
make -j2
make install
}

export CPU=mips
PREFIX=$(pwd)/android/$CPU 
build_one
cp Android.mk $PREFIX/Android.mk
cd $PROJECT_JNI
export ABI=$CPU
export PLATFORM="android-14"
ndk-build
cp -r "$PROJECT_LIBS/$CPU" "$PROJECT_LIBS/../out" 
cd $DIR
