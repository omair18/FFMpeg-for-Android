#!/bin/bash
SYSROOT=$NDK/platforms/android-14/arch-arm/
TOOLCHAIN=$NDK/toolchains/arm-linux-androideabi-4.9/prebuilt/darwin-x86_64
rm -f $(pwd)/compat/strtod.o
function build_one
{
./configure --prefix=$PREFIX $COMMON $CONFIGURATION --cross-prefix=$TOOLCHAIN/bin/arm-linux-androideabi- --target-os=linux --cpu=armv5te --arch=arm --disable-asm --enable-armv5te --disable-stripping --extra-cflags="-O3 -Wall -pipe -ffast-math -fstrict-aliasing -Werror=strict-aliasing -Wno-psabi -Wa,--noexecstack -DANDROID -DNDEBUG-march=armv5te -mtune=arm9tdmi -msoft-float $ADDI_CFLAGS" --extra-ldflags="$ADDI_LDFLAGS" --sysroot=$SYSROOT

make clean
make -j2 
make install
}

export CPU=armeabi
PREFIX=$(pwd)/android/$CPU 
build_one
python FFmpegParser.py -d $PROJECT_JNI
cp Android.mk $PREFIX/Android.mk
cd $PROJECT_JNI
export ABI=$CPU
export PLATFORM="android-14"
ndk-build
cp -r "$PROJECT_LIBS/$CPU" "$PROJECT_LIBS/../out" 
cd $DIR

