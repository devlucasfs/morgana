#include "src/codegen/interface.hpp"
#include "src/externs.hpp"
#include <filesystem>
#include <string>
#include <cstdlib>
#include "libs.hpp"
#include "samar.hpp"

class x86_64_linux_behaviour : public behaviour_interface_methods {
public:
    using behaviour_interface_methods::behaviour_interface_methods;
    bool supports_libs() const override { return true; }
    void build(char *sake, std::filesystem::path path, std::string output, std::string extra) override {
        auto _asm     = path / "asm.s";
        auto  obj     = path / "m.o";
        auto  bin     = path / "bin";
        auto  archive = path / "archive.a";
        auto  shared  = path / "shared.so";
        std::string link_flags = linker_flags(output);
        SamarState *samar = __samar_load(sake);
        __samar_push_arg   (samar, (char*) "asm",     (char*) _asm.c_str());
        __samar_push_arg   (samar, (char*) "obj",     (char*) obj.c_str());
        __samar_push_arg   (samar, (char*) "bin",     (char*) bin.c_str());
        __samar_push_arg   (samar, (char*) "archive", (char*) archive.c_str());
        __samar_push_arg   (samar, (char*) "shared",  (char*) shared.c_str());
        __samar_push_arg   (samar, (char*) "libs",    (char*) link_flags.c_str());
        __samar_call_runner(samar, (char*) ("x86_64::linux::" + runner(output)).c_str());
    }
private:
    static std::string sh_quote(const std::string &s) {
        std::string r = "'";
        for( char c : s ) {
            if( c == '\'' ) r += "'\\''";
            else            r += c;
        }
        return r + "'";
    }
    std::string linker_flags(const std::string &output) const {
        if( libs.empty() || output == "static-archive" ) return "";

        auto lib_dir = (std::filesystem::current_path() / "libs").string();

        const char* home = std::getenv("HOME");
        auto home_lib = home
            ? (std::filesystem::path(home) / ".carla" / "shared").string()
            : std::string{};

        std::string flags = "-L " + sh_quote(lib_dir);
        if( !home_lib.empty() )
            flags += " -L " + sh_quote(home_lib);

        bool has_shared = false;
        for( const auto &lib : libs ) {
            switch(lib.kind) {
                case lib_kind::shared:
                    flags += " -l" + lib.name;
                    has_shared = true;
                    break;
                case lib_kind::static_:
                    flags += " -Bstatic -l" + lib.name + " -Bdynamic";
                    break;
            }
        }
        if( has_shared && output == "native-bin" ) {
            flags += " -rpath " + sh_quote(lib_dir);
            if( !home_lib.empty() )
                flags += " -rpath " + sh_quote(home_lib);
            flags += " -dynamic-linker /lib64/ld-linux-x86-64.so.2";
        }
        return flags;
    }
};
