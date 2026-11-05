#pragma once

#include "../compiler/codegen/backends.hpp"
#include "../externs.hpp"
#include <filesystem>
#include <vector>

struct extra_d {
    std::vector<std::string> files;
    std::string extra_file;

    extra_d() = default;
    extra_d(
        std::string output_file,
        std::vector<std::string> files
    ) : files(files), extra_file(output_file) {}
}

class assembly_behaviour {
public:
    target_t target;
    explicit assembly_behaviour(target_t target) : target(target) {};
    virtual ~assembly_behaviour() = default;

    virtual void build_fail(std::string msg) {
        CompilerOutputs::fatal("Fail when to build: " + msg);
    };

    virtual int build(EvaParser *eva, std::string ns, std::filesystem::path path, extra_d data) { return 0; };
    virtual int link( EvaParser *eva, std::string ns, std::filesystem::path path, extra_d data)  { return 0; };
    virtual int then( EvaParser *eva, std::string ns, std::filesystem::path path, extra_d data)  { return 0; };
};
