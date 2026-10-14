#include "./mangled.hpp"
#include "./download.hpp"
#include "./compiler/lexer.hpp"
#include "./compiler/parser.hpp"
#include "./compiler/codegen/handler.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

extern "C" void
mangled(build) (cstring mpath, cstring target, cstring runa, cstring libs, cstring out)
{   std::string targetf(target), libraries(libs), output(out), extensor(runa);
    fs::path target_folder  = fs::path(targetf);
    fs::path clib_folder    = target_folder / MCLIBC_REPO;
    fs::path objects_folder = target_folder / "objects";
    fs::path output_file    = target_folder / output;
    fs::create_directory(objects_folder);
    fs::create_directory(clib_folder);

    std::stringstream stream(libraries);
    std::string lib;
    while(std::getline(stream, lib, ','))
    {   fs::path header = clib_folder / (lib + ".h");
        fs::path src = clib_folder / (lib + ".c");
        fs::path obj = objects_folder / (lib + ".o");

        if( fs::exists(obj) ) continue;

        curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".c", src.string());
        curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".h", header.string());

        std::stringstream ss;
        ss << "clang -c \"" << src.string() << "\" -o \"" << obj.string() << "\" -O3 -ffreestanding -fno-builtin";
        std::system(ss.str().c_str());
    }

    Lexer lexer(mpath);
    parser(lexer, output, extensor);
}
