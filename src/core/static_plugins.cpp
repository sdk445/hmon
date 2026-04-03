#include "hmon/static_plugins.hpp"

#include <vector>

namespace hmon::core {

std::vector<StaticPlugin>& staticPlugins() {
    static std::vector<StaticPlugin> plugins;
    return plugins;
}

}
