#include "./mangled.hpp"
#include "./compiler/lexer.hpp"
#include "./compiler/parser.hpp"
#include "./hash.hpp"
#include "./compiler_outputs.hpp"
#include "./externs.hpp"
#include "./toolchain/behaviours.hpp"
#include "./toolchain/factory.hpp"
#include "target.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <typeinfo>

namespace fs = std::filesystem;

std::vector<std::string> get_seva_directives(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};

    std::stringstream ss;
    ss << f.rdbuf();
    std::istringstream stream(ss.str());

    std::vector<std::string> directives;
    std::string line;
    while (std::getline(stream, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] != '@') continue;

        const auto end = line.find_last_not_of(" \t\r");
        directives.emplace_back(line.substr(start + 1, end - start));
    }
    return directives;
}

bool cli_exists(const std::string& cli) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

    const std::string path(path_env);
#ifdef _WIN32
    const char separator = ';';
    const char* extensions_env = std::getenv("PATHEXT");
    const std::string extensions = extensions_env ? extensions_env : ".EXE;.CMD;.BAT;.COM";
#else
    const char separator = ':';
#endif

    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, separator)) {
        if (dir.empty()) continue;

#ifdef _WIN32
        std::stringstream ext_ss(extensions);
        std::string ext;
        while (std::getline(ext_ss, ext, ';')) {
            if (fs::exists(fs::path(dir) / (cli + ext)))
                return true;
        }
#endif
        if (fs::exists(fs::path(dir) / cli))
            return true;
    }
    return false;
}

auto prepare_eva(const fs::path& seva_file, std::string_view sake) {
    set(const_cast<char*>(sake.data()));
    setoutput(const_cast<char*>(seva_file.string().c_str()));

    if (prepare() != 0) {
        std::fprintf(stderr, "[morgana] exception! Sake file can't be translated to Eva.\n");
        std::exit(14);
    }
    return eva_make_parser(seva_file.c_str());
}

std::string find_matching_namespace(EvaParser *eva, const fs::path& seva_file,
                                    std::string_view arch_os) {
    std::string s_arch, s_os;
    if (const auto pos = arch_os.find('-'); pos != std::string_view::npos) {
        s_arch = arch_os.substr(0, pos);
        s_os   = arch_os.substr(pos + 1);
    } else {
        s_arch = std::string(arch_os);
        s_os   = "none";
    }

    for (const auto& ns : get_seva_directives(seva_file.string())) {
        auto t    = eva_get_value_from_namespace(eva, ns.c_str(), "target");
        auto arch = eva_get_map_field(t, "arch");
        auto os   = eva_get_map_field(t, "os");

        if (arch.tag != eva_string || os.tag != eva_string) continue;

        if (std::string_view(arch.data.string) == s_arch &&
            std::string_view(os.data.string)   == s_os) {
            return ns;
        }
    }

    std::fprintf(stderr, "[morgana] exception! Invalid Sake file\n");
    std::exit(14);
}

void ensure_tools(auto eva, std::string_view ns) {
    auto tools = eva_get_value_from_namespace(eva, ns.data(), "tools");
    const int len = eva_get_list_length(tools);

    for (int i = 0; i + 1 < len; i += 2) {
        auto cli = eva_get_list_field(tools, i);
        auto src = eva_get_list_field(tools, i + 1);

        if (cli.tag != eva_string || src.tag != eva_string) continue;
        if (cli_exists(cli.data.string)) continue;

        const std::string name(cli.data.string);
        std::string instructions;

        switch (hash(std::string(src.data.string))) {
            case hash("package"):
                instructions = "using your package manager.\n└─ E.g. sudo pacman -S " + name;
                break;
            default:
                instructions = "it manually.";
                break;
        }
        CompilerOutputs::fatal("`" + name + "` isn't downloaded. Try to install " + instructions);
    }
}

std::unique_ptr<assembly_behaviour> make_behaviour(const target_t& config) {
    try {
        return behaviour_factory::create(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[morgana] exception! (%s) : %s\n",
                     typeid(e).name(), e.what());
        std::exit(14);
    }
}


extern "C" void
mangled(upload)(cstring target, cstring sake_c,
                cstring ao, cstring baudrate, cstring device)
{
    const fs::path target_folder(target);
    const fs::path seva_file = target_folder / "Sake.eva";

    auto eva     = prepare_eva(seva_file, sake_c);
    auto decided = find_matching_namespace(eva, seva_file, ao);

    ensure_tools(eva, decided);

    target_t config;
    gen_target(&config, ao);

    auto behaviour = make_behaviour(config);
    behaviour->upload(eva, decided, target_folder, baudrate, device);
}

extern "C" void
mangled(build)(cstring mpath, cstring target, cstring ao,
               cstring libs, cstring sake_c, bool use_ram,
               bool enable_then = false)
{
    const fs::path target_folder(target);
    const fs::path seva_file   = target_folder / "Sake.eva";
    const fs::path asm_file    = target_folder / "asm.s";
    const fs::path output_file = target_folder / "bin";
    const fs::path output_object = target_folder / "m.o";

    auto eva     = prepare_eva(seva_file, sake_c);
    auto decided = find_matching_namespace(eva, seva_file, ao);

    ensure_tools(eva, decided);

    target_t config;
    try {
        Lexer lexer(mpath);
        config = parser(lexer, asm_file.string(), ao, use_ram);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[morgana] exception! (%s) : %s\n",
                     typeid(e).name(), e.what());
        std::exit(14);
    }

    auto behaviour = make_behaviour(config);

    extra_d ex(output_file.string(), output_object.string());
    behaviour->build(eva, decided, target_folder);
    behaviour->link(eva, decided, target_folder);
}
