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
string(REPLACE "\\" "\\\\" text "${text}")
string(REPLACE "\"" "\\\"" text "${text}")
string(REPLACE "\n" "\\n\"\n\"" text "${text}")
file(WRITE ${OUT} "\"${text}\"\n")
