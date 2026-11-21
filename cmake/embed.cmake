# Turn a binary file into a C array header: cmake -DIN=.. -DOUT=.. -DNAME=.. -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
file(WRITE "${OUT}" "// generated from ${IN}\n#pragma once\nstatic const unsigned char ${NAME}[] = {${arr}};\n")
