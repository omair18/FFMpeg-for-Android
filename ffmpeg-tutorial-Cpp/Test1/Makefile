#
# http://www.gnu.org/software/make/manual/make.html
#
CC:=g++
INCLUDES:=$(shell pkg-config --cflags libavformat libavcodec libswresample libswscale libavutil sdl opencv)
CFLAGS:=-Wall -ggdb
LDFLAGS:=$(shell pkg-config --libs libavformat libavcodec libswresample libswscale libavutil sdl opencv) -lm
EXE:=bin/tutorial01.out

#
# This is here to prevent Make from deleting secondary files.
#
.SECONDARY:
	

#
# $< is the first dependency in the dependency list
# $@ is the target name
#
all: dirs $(EXE)

dirs:
	mkdir -p obj
	mkdir -p bin

$(EXE): ./src/Test1.cpp
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $(EXE)

tags: src/*.cpp
	ctags src/*.cpp

bin/%.out: obj/%.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

obj/%.o : %.cpp
	$(CC) $(CFLAGS) $< $(INCLUDES) -c -o $@

clean:
	rm -f obj/*
	rm -f bin/*
	rm -f tags

