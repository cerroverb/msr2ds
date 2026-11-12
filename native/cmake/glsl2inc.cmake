# A shader's GLSL as a C string literal, for the OpenGL presenter (runtime/video_gl.c), which compiles it at run time.
# A line `#include "x"` is replaced by the file x beside the shader, as glslc's GL_GOOGLE_include_directive does, and
# comment lines and blank lines are left out (FidelityFX's headers are mostly comments).
#   cmake -DIN=<shader> -DOUT=<file.inc> -P glsl2inc.cmake
get_filename_component(IN ${IN} ABSOLUTE)
get_filename_component(dir ${IN} DIRECTORY)
file(READ ${IN} text)
string(REGEX MATCHALL "\n#include \"[^\"]+\"" includes "\n${text}")
foreach(inc ${includes})
    string(REGEX REPLACE "\n#include \"([^\"]+)\"" "\\1" name "${inc}")
    file(READ ${dir}/${name} body)
    string(REPLACE "#include \"${name}\"" "${body}" text "${text}")
endforeach()
string(REPLACE "\r" "" text "${text}")
string(REGEX REPLACE "\n[ \t]*//[^\n]*" "" text "${text}")   # whole-line comments (GLSL has no strings for // to be in)
string(REGEX REPLACE "\n[ \t]*\n" "\n" text "${text}")
string(REGEX REPLACE "\n[ \t]*\n" "\n" text "${text}")
# A hex literal of 8 digits from 0x80000000 up, with no u: FidelityFX's AU1_(0x80000000). GLSL takes it as an int's
# bits, but Intel's older compilers (HD 4600, driver 20.19.15) fail it with ERROR___INTEGER_CONST_OVERFLOW, and every
# shader compiled after that fails too, the overlay's among them (issue #57). As a uint it means the same.
set(hex "[0-9a-fA-F]")
string(REGEX REPLACE "(0[xX][89a-fA-F]${hex}${hex}${hex}${hex}${hex}${hex}${hex})([^uU0-9a-fA-F])" "\\1u\\2" text "${text}")
string(REPLACE "\\" "\\\\" text "${text}")
string(REPLACE "\"" "\\\"" text "${text}")
string(REPLACE "\n" "\\n\"\n\"" text "${text}")
file(WRITE ${OUT} "\"${text}\"\n")
