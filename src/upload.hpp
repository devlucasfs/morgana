#include <filesystem>
#include "codegen/make.hpp"

#include "general.hpp"
#include "samar.hpp"


extern "C"
void mangled(upload) (cstring target, cstring sake, cstring extensor, cstring baudrate, cstring device, cstring mcu) try_pattern({
    std::filesystem::path target_folder(target);
    auto behaviour = behaviours(extensor);
    behaviour->upload((char*)sake, target_folder, baudrate, device, mcu);
})
