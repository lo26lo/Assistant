#include "utils/Paths.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace ibom::utils {

fs::path dataDir()
{
    // 1. Explicit override (Docker mounts a single volume here).
    if (const char* env = std::getenv("IBOM_DATA_DIR"); env && *env) {
        fs::path p(env);
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }

    // 2. Per-user location — matches Qt QStandardPaths::AppDataLocation.
    if (const char* home = std::getenv("HOME"); home && *home) {
        fs::path p = fs::path(home) / ".local" / "share" / "MicroscopeIBOM";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }

    // 3. Last resort.
    return fs::current_path();
}

fs::path dataSubDir(const std::string& sub)
{
    fs::path p = dataDir() / sub;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

} // namespace ibom::utils
