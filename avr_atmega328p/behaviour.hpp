#include "src/codegen/interface.hpp"
#include "src/externs.hpp"

class avr_atmega328p_behaviour : public behaviour_interface_methods {
    using behaviour_interface_methods::behaviour_interface_methods;

    int build(EvaParser *eva, std::string ns, std::filesystem::path path) override {
        auto build = eva_get_value_from_namespace(eva, ns.c_str(), "compiling");
        if( build.tag != eva_string ) build_fail("Sake: Invalid build field format.");

        std::string cpp(build.data.string);
        eva_dump_pointer(build);

        auto path_elf  = path / "avr.elf",
             path_hex  = path / "avr.hex",
             path_file = path / "asm.s";

        auto elf  = path_elf.string(),
             hex  = path_hex.string(),
             file = path_file.string();

        replace(cpp, "file", file);
        replace(cpp, "elf", elf);
        replace(cpp, "hex", hex);

        return std::system(cpp.c_str());
    }

    int upload(EvaParser *eva, std::string ns, std::filesystem::path path, std::string baudrate, std::string device) override {
        auto upload = eva_get_value_from_namespace(eva, ns.c_str(), "upload");
        if( upload.tag != eva_string ) build_fail("Sake: Invalid upload field format.");

        std::string cpp(upload.data.string);
        eva_dump_pointer(upload);

        auto path_hex = path / "avr.hex";
        auto hex = path_hex.string();

        baudrate = (baudrate.empty()) ? "115200" : baudrate;
        device = (device.empty()) ? "/dev/ttyUSB0" : device;

        replace(cpp, "baudrate", baudrate);
        replace(cpp, "device", device);
        replace(cpp, "hex", hex);

        return std::system(cpp.c_str());
    }
};
