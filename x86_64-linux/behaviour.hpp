#include "src/codegen/interface.hpp"
#include "src/externs.hpp"

#include "samar.hpp"

class x86_64_linux_behaviour : public behaviour_interface_methods {
public:
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string extra) override {
        auto _asm = path / "asm.s";
        auto  obj = path / "m.o";
        auto  bin = path / "bin";

        SamarState *samar = __samar_load(sake);

        __samar_push_arg   (samar, (char*) "asm", (char*) _asm.c_str());
        __samar_push_arg   (samar, (char*) "obj", (char*) obj.c_str());
        __samar_push_arg   (samar, (char*) "bin", (char*) bin.c_str());
        __samar_call_runner(samar, (char*) "x86_64::linux::native_bin");
    }
};
