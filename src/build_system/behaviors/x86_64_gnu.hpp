#pragma once

#include "../backends.hpp"
#include "../../externs.hpp"
#include "../common.hpp"
#include <cstdlib>

namespace fs = std::filesystem;
class build_x86_64_gnu : public assembly_behaviour {
    using assembly_behaviour::assembly_behaviour;

    int build(EvaParser *eva, std::string ns, fs::path path) override {
        auto build = eva_get_value_from_namespace(eva, ns.c_str(), "compiling");
        if( build.tag != eva_string ) build_fail("Sake: Invalid build field format.");

        std::string cpp(build.data.string);
        eva_dump_pointer(build);

        auto file = path / "asm.s";
        auto middle = path / "m.o";

        replace(cpp, "file", file);
        replace(cpp, "output", middle.string());

        return std::system(cpp.c_str());
    }

    int link(EvaParser *eva, std::string ns, fs::path path) override {
        auto linking = eva_get_value_from_namespace(eva, ns.c_str(), "linking");
        if( linking.tag != eva_string ) build_fail("Sake: Invalid linking field format.");

        std::string cpp(linking.data.string);
        eva_dump_pointer(linking);
        auto middle = path / "m.o";
        auto output = path / "bin";

        replace(cpp, "file", middle.string());
        replace(cpp, "output", output.string());

        return std::system(cpp.c_str());
    }
};
