#include "./mangled.hpp"
#include "./download.hpp"
#include "./compiler/lexer.hpp"
#include "./compiler/parser.hpp"
#include "./compiler/hash.hpp"
#include "./compiler_outputs.hpp"
#include "./externs.hpp"
#include "build_system/backends.hpp"
#include "build_system/behaviors/factory.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <filesystem>
#include <string_view>
#include <typeinfo>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

std::vector<std::string> get_seva_directives(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::istringstream stream(ss.str());

    std::vector<std::string> directives;
    std::string line;

    while( std::getline(stream, line) ) {
        size_t start = line.find_first_not_of(" \t\r");
        if( start == std::string::npos || line[start] != '@' ) continue;

        size_t end = line.find_last_not_of(" \t\r");
        directives.push_back(line.substr(start + 1, end - start));
    }

    return directives;
}

namespace fs = std::filesystem;

bool cli_exists(const std::string& cli) {
    const char* path_env = std::getenv("PATH");
    if(! path_env ) return false;
    std::string path(path_env);
#ifdef _WIN32
    const char separator = ';';
    const char* extensions_env = std::getenv("PATHEXT");
    std::string extensions = extensions_env ? extensions_env : ".EXE;.CMD;.BAT;.COM";
#else
    const char separator = ':';
#endif
    std::stringstream ss(path);
    std::string dir;
    while(std::getline(ss, dir, separator)) {
        if( dir.empty() ) continue;
#ifdef _WIN32
        std::stringstream ext_ss(extensions);
        std::string ext;
        while(std::getline(ext_ss, ext, ';'))
            if( fs::exists(fs::path(dir) / (cli + ext)) )
                return true;
#endif
        if( fs::exists(fs::path(dir) / cli) ) return true;
    }

    return false;
}


extern "C" void
mangled(build) (cstring mpath, cstring target, cstring ao, cstring libs, cstring sake_c, bool use_ram, bool enable_then = false)
{   std::string targetf(target), libraries(libs), arch_os(ao), sake(sake_c);
    fs::path target_folder  = fs::path(targetf);
    fs::path output_object  = target_folder  / "m.o";
    fs::path seva_file      = target_folder  / "Sake.eva";
    fs::path asm_file       = target_folder  / "asm.s";
    fs::path output_file    = target_folder  / "bin";

    set((char*) sake.c_str());
    setoutput((char*) seva_file.string().c_str());

    if( prepare() != 0 ) {
        std::fprintf(stderr, "[morgana] exception! Sake file can't be translated to Eva.\n");
        std::exit(14);
    }

//     std::stringstream stream(libraries);
//     std::string lib;
//     while( std::getline(stream, lib, ',') )
//     {   fs::path header = clib_folder / (lib + ".h");
//         fs::path src = clib_folder / (lib + ".c");
//         fs::path obj = objects_folder / (lib + ".o");
//
//         if( fs::exists(obj) ) continue;
//
//         curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".c", src.string());
//         curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + lib + ".h", header.string());
//
//         std::ifstream file(src);
//         std::string line;
//         if( std::getline(file, line) && line.rfind("//h: ", 0) == 0 )
//         {   std::stringstream headers(line.substr(5));
//             std::string name;
//             while( std::getline(headers, name, ' ') )
//             {   if( name.empty() ) continue;
//
//                 fs::path dependency = clib_folder / (name + ".h");
//                 if( fs::exists(dependency) ) continue;
//
//                 curl_download("https://raw.githubusercontent.com/Carla-repos/mclibc/main/lib/" + name + ".h", dependency.string());
//             }
//         }
//
//         std::stringstream ss;
//         ss << "clang -c \"" << src.string() << "\" -o \"" << obj.string() << "\" -O3 -ffreestanding -fno-builtin";
//         std::system(ss.str().c_str());
//     }

    Lexer lexer(mpath);

    target_t config;
    try {
        config = parser(lexer, asm_file.string(), arch_os, use_ram);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[morgana] exception! (%s) : %s\n", typeid(e).name(), e.what());
        std::exit(14);
    }

    std::stringstream ss;
    auto append = [&](std::string_view v) {
        if(! ss.str().empty() ) ss << " && ";
        ss << v;
    };

    auto eva = eva_make_parser(seva_file.c_str());
    append("");

    size_t pos = arch_os.find('-');
    std::string s_arch, s_os;

    if( pos != std::string::npos ) {
        s_arch = arch_os.substr(0, pos);
        s_os   = arch_os.substr(pos + 1);
    }
    else {
        s_arch = arch_os;
        s_os   = "none";
    }

    std::string decided = "_void";
    auto nss = get_seva_directives(seva_file.string());
    for( auto& ns : nss ) {
        auto t = eva_get_value_from_namespace(eva, ns.c_str(), "target");

        auto arch = eva_get_map_field(t, "arch");
        auto os = eva_get_map_field(t, "os");
        if( arch.tag != eva_string || os.tag != eva_string ) continue;

        if(
            std::string(arch.data.string) == s_arch &&
            std::string(os.data.string)   == s_os
        ) decided = ns;
    }

    if( decided == "_void" ) {
        std::fprintf(stderr, "[morgana] exception! Invalid Sake file\n");
        std::exit(14);
    }

    auto tools = eva_get_value_from_namespace(eva, decided.c_str(), "tools");

    for( int i = 0; i + 1 < eva_get_list_length(tools); i += 2 ) {
        auto cli = eva_get_list_field(tools, i);
        auto src = eva_get_list_field(tools, i + 1);

        if( cli.tag != eva_string || src.tag != eva_string ) continue;
        if( cli_exists(cli.data.string) ) continue;

        std::string name(cli.data.string);
        std::string instructions;

        switch( hash(std::string(src.data.string)) ) {
            case hash("package"): instructions = "using your package manager.\n└─ E.g. sudo pacman -S " + name;
            break;

            default: instructions = "it manually.";
            break;
        }

        CompilerOutputs::fatal("`" + name + "` isn't downloaded. Try to install " + instructions);
    }

    std::unique_ptr<assembly_behaviour> behaviour;
    try {
        behaviour = behaviour_factory::create(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[morgana] exception! (%s) : %s\n", typeid(e).name(), e.what());
        std::exit(14);
    }

    extra_d ex(output_file.string(), output_object.string());

    behaviour->build(eva, decided, target_folder);
    behaviour->link(eva, decided, target_folder);
    // if( enable_then ) behaviour->then(eva, decided, target_folder);
}
