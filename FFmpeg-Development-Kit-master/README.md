# FFmpeg Development Kit
Main purpose of this repository is to help people in building FFmpeg, mainly on mobile platforms
(I tested this on Android, but armv7 shared libraries should be ok for iOS too).

## Prerequisites
This kit is suposed to work with Android NDK version **r14b for Linux or OSX**. Version of FFmpeg in repo is **3.2.4**. Please, note that 
versions are quite important here, since it's not guaranteed that with other version of FFmpeg and/or NDK shared libs 
would even compile. If you want to change version of FFmpeg and/or NDK you're completely free to do this, althought I should aware you 
that this might be not a good idea. 

## Setup
Quite small amount of setup is needed, basically all you need is to download appropriate NDK version. As written above, I've used 
version r14b for Linux.
As soon as you got NDK, unpack it to disc (please, note, that Linux version have fromat .bin which would require 7z from you), then 
**place FFmpeg from this repo under NDK/sources** (so path to FFmpeg would be NDK/sources/ffmpeg), JNI (Android project for NDK-build) 
**have to be in the same directory in which NDK is located**. 

Also, I have written JNI wrapper which is called videokit.c (it's hopefully soon will be released thought Maven
as separate Android library) and you have to change signature of function "run" in order to match class' package from 
which you will call native code. Now signature is: Java_processing_ffmpeg_videokit_VideoKit_run which is corresponds to: 
processing.ffmpeg.videokit.VideoKit class. **Don't forget to change it to your package or your Java part of code will not work**.

It's pretty much all. FFmpeg directory contains 6 bash-scripts: build_all.sh, buid_armeabi.sh (armv6), build_armeabi-v7a.sh (arm7a), 
build_arm64-v8a (arm8 x64), build_mips.sh, build_x86.sh **(will require yasm from you on linux)**. 
**I highly encourage you to use build_all.sh since it have common setup 
between scripts. If you want to build only for some specific architectures - just comment out appropriate build_* in build_all.sh
(by default it will compile everything)**.

After this you should be able to just run ./build_all.sh if FFMpeg directory. Please, note, building normally is taking quite a 
bit of time, so be ready to wait approx 2h for everything to be builded. After this, inside FFmpeg directory you will have 
"android" folder which will contain .so files for every architecture and inside JNI/app you should have folder called "out" 
which should contain libvideokit.so (unless you have changed the name of library) and all related .so files for every platform. 
**On the latest FFmpeg and NDK I got problems with compiling of libraries for MIPS architecture**. If you really need that one - use version of FFmpeg 2.8.4 on this repo on accroding branch.

## License
FFmpeg by itself is licensed under LGPL. FFmpeg folder contains **unchanged** code of FFmpeg of version 2.8.4, all changes 
is done in files inside JNI/app/jni folder. Code that was written by me is licensed under LGPL too, so you are free to use it.

**Important note: libx264, which is also distributed inside FFmpeg folder, is licensed under GPL. If you will use this development 
kit in order to build FFmpeg with any of GPL libraries (as libx264 for example) end product will be licensed under GPL. 
If none of GPL libraries were used: product is licensed under LGPL too. Read FFmpeg license to find out more: 
[Legal](https://ffmpeg.org/legal.html)**
