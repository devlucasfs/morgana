#include "samar.hpp"
#include "src/codegen/interface.hpp"
#include "src/externs.hpp"

class avr_atmega328p_behaviour : public behaviour_interface_methods {
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string extra) override {
        auto path_elf  = path / "avr.elf",
             path_hex  = path / "avr.hex",
             path_file = path / "asm.s";

        auto elf  = path_elf.string(),
             hex  = path_hex.string(),
             file = path_file.string();

        SamarState *samar = __samar_load(sake);
        auto runner = "avr_atmega328p::" + extra + "::native_bin";

        __samar_push_arg   (samar, (char*) "elf",  (char*) elf.c_str());
        __samar_push_arg   (samar, (char*) "hex",  (char*) hex.c_str());
        __samar_push_arg   (samar, (char*) "file", (char*) file.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }

    void upload(char *sake, std::filesystem::path path, std::string baudrate, std::string device, std::string mcu="arduino") override {
        auto path_hex = path / "avr.hex";
        auto hex = path_hex.string();

        baudrate = (baudrate.empty()) ? "115200" : baudrate;
        device = (device.empty()) ? "/dev/ttyUSB0" : device;

        SamarState *samar = __samar_load(sake);
        auto runner = "avr_atmega328p::" + mcu + "::upload";

        __samar_push_arg   (samar, (char*) "device",   (char*) device.c_str());
        __samar_push_arg   (samar, (char*) "baudrate", (char*) baudrate.c_str());
        __samar_push_arg   (samar, (char*) "hex",      (char*) hex.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }
};
