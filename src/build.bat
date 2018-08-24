@echo off

mkdir ..\build
pushd ..\build

rem Compiler Switches
rem /Zi -- Enable debug
rem /FC -- Display full path of source code files passed to cl.exe in diagnostic text


cl /FC /Zi /Femixer /I ..\src\include ..\src\main.c /link /LIBPATH:..\src\lib avutil.lib swscale.lib swresample.lib avcodec.lib avformat.lib avdevice.lib avfilter.lib

popd
