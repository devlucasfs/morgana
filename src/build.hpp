#include <cstdio>
#include <filesystem>

#include "general.hpp"

#include "recognize/lexer.hpp"
#include "recognize/parser.hpp"
#include "codegen/make.hpp"

extern "C"
void mangled(build) (cstring cfile, cstring ctarget, cstring extensor, cstring sake, cstring extra) try_pattern({
    std::string file(cfile);
    std::string target(ctarget);

    std::filesystem::path target_folder(target);
    std::filesystem::path file_path(file);
    auto asm_file  = target_folder / "asm.s";
    auto output    = std::fopen(asm_file.c_str(), "w+");
    auto tks       = lexer(file_path);
    auto ins       = parser(file_path, tks);

    codegen(extensor, output, ins);
    std::fclose(output);

    auto behaviour = behaviours(extensor);
    behaviour->build((char*)sake, target_folder, extra);
})
