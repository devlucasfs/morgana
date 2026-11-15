#include <cstdio>
#include <filesystem>
#include "general.hpp"

#include "recognize/lexer.hpp"
#include "recognize/parser.hpp"
#include "codegen/make.hpp"

extern "C"
void mangled(build) (cstring cfile) try_pattern({
    std::string file(cfile);
    std::filesystem::path file_path(file);
    auto tks = lexer(file_path);
    auto ins = parser(file_path, tks); // instruction set

    FILE *output = std::fopen("./test.out", "w+");
    codegen("x86_64-gnu", output, ins);
    std::fclose(output);

})
