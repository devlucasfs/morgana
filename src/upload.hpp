#include <filesystem>
#include "codegen/make.hpp"

#include "general.hpp"
#include "samar.hpp"


extern "C"
void mangled(upload) (cstring target, cstring sake, cstring extensor, cstring baudrate, cstring device) try_pattern({
    const fs::path target_folder(target);
    const fs::path seva_file = target_folder / "Sake.eva";

    auto eva     = prepare_eva(seva_file, sake);
    auto decided = find_matching_namespace(eva, seva_file, extensor);

    ensure_tools(eva, decided);

    auto behaviour = behaviours(extensor);
    behaviour->upload(eva, decided, target_folder, baudrate, device);
})
