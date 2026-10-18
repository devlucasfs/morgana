#include "./mangled.hpp"
#include "./download.hpp"
#include "./compiler/lexer.hpp"
#include "./compiler/parser.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

extern "C" void
mangled(build) (cstring mpath, cstring target, cstring runa, cstring libs)
{   std::string targetf(target), libraries(libs), extensor(runa);
    fs::path target_folder  = fs::path(targetf);
    fs::path clib_folder    = target_folder / MCLIBC_REPO;
    fs::path objects_folder = target_folder / "objects";
    fs::path output_object  = objects_folder / "m.o";
    fs::path asm_file       = target_folder / "asm.s";
    fs::path output_file    = target_folder / "bin";
    fs::create_directory(objects_folder);
    fs::create_directory(clib_folder);

    std::stringstream stream(libraries);
    std::string lib;

    while( std::getline(stream, lib, ',') )
    {   fs::path header = clib_folder / (lib + ".h");
        fs::path src = clib_folder / (lib + ".c");
        fs::path obj = objects_folder / (lib + ".o");

        if( fs::exists(obj) ) continue;

        curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".c", src.string());
        curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".h", header.string());

        std::ifstream file(src);
        std::string line;
        if( std::getline(file, line) && line.rfind("//h: ", 0) == 0 )
        {   std::stringstream headers(line.substr(5));
            std::string name;
            while( std::getline(headers, name, ' ') )
            {   if( name.empty() ) continue;

                fs::path dependency = clib_folder / (name + ".h");
                if( fs::exists(dependency) ) continue;

                curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + name + ".h", dependency.string());
            }
        }

        std::stringstream ss;
        ss << "clang -c \"" << src.string() << "\" -o \"" << obj.string() << "\" -O3 -ffreestanding -fno-builtin";
        std::system(ss.str().c_str());
    }

    Lexer lexer(mpath);
    parser(lexer, asm_file.string(), extensor);

    std::vector<std::string> objects;
    for( const auto& entry : fs::directory_iterator(objects_folder) )
        if( entry.is_regular_file() && entry.path().extension() == ".o" )
            objects.push_back(entry.path().string());

    std::stringstream ss;
    ss << "as \"" << asm_file.string() << "\" -o \"" << output_object.string() << "\"";
    std::system(ss.str().c_str());
    ss.str(""); ss.clear();

    ss << "ld -o \"" << output_file.string() << "\"";
    for( const auto& object : objects )
        ss << " \"" + object + "\"";

    std::system(ss.str().c_str());
    ss.str(""); ss.clear();

    ss << "chmod +x \"" << output_file.string() << "\"";
    std::system(ss.str().c_str());

}
