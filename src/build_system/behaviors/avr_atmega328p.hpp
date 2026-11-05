#pragma once

#include "../backends.hpp"
#include "../../externs.hpp"
#include "../common.hpp"
#include <cstdlib>

namespace fs = std::filesystem;
class build_avr_atmega328p : public assembly_behaviour {
    using assembly_behaviour::assembly_behaviour;

    int build(EvaParser *eva, std::string ns, fs::path path) override {
        auto build = eva_get_value_from_namespace(eva, ns.c_str(), "compiling");
        if( build.tag != eva_string ) build_fail("Sake: Invalid build field format.");

        std::string cpp(build.data.string);
        eva_dump_pointer(build);

        auto file = target.output;
        auto path_elf = path / "avr.elf",
             path_hex = path / "avr.hex";

        auto elf = path_elf.string(),
             hex = path_hex.string();

        replace(cpp, "file", file);
        replace(cpp, "elf", elf);
        replace(cpp, "hex", hex);

        return std::system(cpp.c_str());
    }

    int then(EvaParser *eva, std::string ns, fs::path path) override {
        auto upload = eva_get_value_from_namespace(eva, ns.c_str(), "upload");
        if( upload.tag != eva_string ) build_fail("Sake: Invalid upload field format.");

        std::string cpp(upload.data.string);
        eva_dump_pointer(upload);

        auto file = target.output;
        auto path_hex = path / "avr.hex";

        auto hex = path_hex.string();

        replace(cpp, "baudrate", "115200");
        replace(cpp, "device", "/dev/ttyUSB0");
        replace(cpp, "hex", hex);

        return std::system(cpp.c_str());
    }
};
