#include <memory>

#include "modules/core/Core.h"

auto main(int argc, char *argv[]) -> int {
    const auto core = std::make_shared<Core>(4321);
    core->start();
}
