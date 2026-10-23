@echo off
if not exist build mkdir build
cl /nologo /O2 /std:c++20 /EHsc /Isrc src\*.cpp tests\test_simplify.cpp /Fe:build\test_simplify.exe /Fo:build\
