APP_OPTIM := release
APP_PLATFORM := $(PLATFORM)
APP_ABI := $(ABI)
NDK_TOOLCHAIN_VERSION=4.9
APP_PIE := false
APP_STL := stlport_shared

APP_CFLAGS := -O3 -Wall -pipe \
    -ffast-math \
    -fstrict-aliasing -Werror=strict-aliasing \
    -Wno-psabi -Wa,--noexecstack \
    -DANDROID -DNDEBUG
