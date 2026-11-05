#pragma once

#include "../compiler/codegen/backends.hpp"
#include "../externs.hpp"
#include <filesystem>
#include <vector>

struct extra_d {
    std::string end, middle;

    extra_d() = default;
    extra_d(
        std::string end_file,
        std::string middle_file
    ) : end(end_file), middle(middle_file) {}
};

class assembly_behaviour {
public:
    target_t target;
    explicit assembly_behaviour(target_t target) : target(target) {};
    virtual ~assembly_behaviour() = default;

    virtual void build_fail(std::string msg) {
        CompilerOutputs::fatal("Fail when to build: " + msg);
    };

    virtual int build(EvaParser *eva, std::string ns, std::filesystem::path path) { return 0; };
    virtual int link( EvaParser *eva, std::string ns, std::filesystem::path path)  { return 0; };
    virtual int then( EvaParser *eva, std::string ns, std::filesystem::path path)  { return 0; };
};
