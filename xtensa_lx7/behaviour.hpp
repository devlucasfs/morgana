#include "src/codegen/interface.hpp"
#include "src/externs.hpp"
#include <filesystem>

#include "samar.hpp"

class xtensa_lx7_behaviour : public behaviour_interface_methods {
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string output, std::string extra="esp32") override {
        auto path_file = path / "asm.s";
        auto file = path_file.string();

        SamarState *samar = __samar_load(sake);
        auto identifier = ("xtensa_lx7::" + extra + "::" + runner(output));

        __samar_push_arg   (samar, (char*) "target", (char*) path.string().c_str());
        __samar_push_arg   (samar, (char*) "file",   (char*) file.c_str());
        __samar_call_runner(samar, (char*) identifier.c_str());
    }

    void upload(char *sake, std::filesystem::path path, std::string baudrate, std::string device, std::string mcu="esp32") override {
        auto path_idf = path / "carla-idf";
        auto idf = path_idf.string();

        baudrate = (baudrate.empty()) ? "115200" : baudrate;
        device = (device.empty()) ? "/dev/ttyUSB0" : device;

        SamarState *samar = __samar_load(sake);
        auto runner = "xtensa_lx7::" + mcu + "::upload";

        __samar_push_arg   (samar, (char*) "folder",   (char*) idf.c_str());
        __samar_push_arg   (samar, (char*) "device",   (char*) device.c_str());
        __samar_push_arg   (samar, (char*) "baudrate", (char*) baudrate.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }
};
