# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Convert a binary file into a C++ header with an aligned byte array.
# Usage: cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P EmbedFile.cmake

file(READ ${INPUT} hex HEX)
string(LENGTH "${hex}" len)
math(EXPR nbytes "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n\t" bytes "${bytes}")
file(WRITE ${OUTPUT}
	"// Generated from ${INPUT}\n#pragma once\n#include <cstddef>\n"
	"alignas(4) static const unsigned char ${SYMBOL}[] = {\n\t${bytes}\n};\n"
	"static const size_t ${SYMBOL}_size = ${nbytes};\n"
)
