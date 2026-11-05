# A SPIR-V binary as a C initializer list of 32-bit words, as `glslc -mfmt=c` writes it (for builds that only have
# glslangValidator). cmake -DIN=<file.spv> -DOUT=<file.inc> -P spv2inc.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
set(words "")
set(i 0)
while(i LESS n)
    # little-endian bytes b0 b1 b2 b3 -> 0xb3b2b1b0
    string(SUBSTRING "${hex}" ${i} 8 w)
    string(SUBSTRING "${w}" 0 2 b0)
    string(SUBSTRING "${w}" 2 2 b1)
    string(SUBSTRING "${w}" 4 2 b2)
    string(SUBSTRING "${w}" 6 2 b3)
    list(APPEND words "0x${b3}${b2}${b1}${b0}")
    math(EXPR i "${i} + 8")
endwhile()
list(JOIN words "," body)
file(WRITE "${OUT}" "{${body}}\n")
