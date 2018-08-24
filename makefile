# CC specifies which compiler we're using
CC = gcc

# OBJS specifies which files to compile as part of the project
# OBJS = main.c
OBJS = main.c

#COMPILER_FLAGS specifies the additional compilation options we're using
# COMPILER_FLAGS = -w # -w suppresses all warnings

# Include and Library path
# INCLUDE_PATH = -I/usr/local/include
# LIBRARY_PATH = -L/usr/local/lib

# Libraries to link
LIBS_FFMPEG = -lavformat -lavcodec -lavutil -lswscale -lswresample -lm -lpthread
# LIBS = $(LIBS_FFMPEG) $(LIBS_SDL)
LIBS = $(LIBS_SDL) $(LIBS_FFMPEG)

#OBJ_NAME specifies the name of our exectuable
EXE = mixer

# This is the target that compiles our executable
all:
	$(CC) $(OBJS) $(LIBS) -o $(EXE)

# common:
# 	cc -o fftest main.c -I/usr/local/include -L/usr/local/lib  #-Wno-deprecated-declarations
