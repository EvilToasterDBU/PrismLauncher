// SPDX-License-Identifier: GPL-3.0-only
#include "common/EmuFolders.h"

namespace EmuFolders {

#ifdef BIGSCREEN_RESOURCES_DIR
std::string Resources = BIGSCREEN_RESOURCES_DIR;
#else
std::string Resources = "resources";
#endif

}  // namespace EmuFolders
