#include <catch2/catch_test_macros.hpp>

#include "features/BoardLibrary.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace ibom::features;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path()
             / ("ibom_library_test_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

BoardLibraryEntry entry(const std::string& hash, const std::string& title,
                        const std::string& lastOpened)
{
    BoardLibraryEntry e;
    e.hash = hash;
    e.path = "/boards/" + title + ".html";
    e.title = title;
    e.lastOpened = lastOpened;
    e.components = 42;
    return e;
}

} // namespace

TEST_CASE("BoardLibrary — touch upserts and sorts by recency", "[library]")
{
    TempDir tmp;
    BoardLibrary lib;
    REQUIRE(lib.open(tmp.path / "board_library.json"));
    CHECK(lib.count() == 0);

    REQUIRE(lib.touch(entry("aaa111", "alpha", "2026-07-01T10:00:00Z")));
    REQUIRE(lib.touch(entry("bbb222", "beta",  "2026-07-15T10:00:00Z")));
    REQUIRE(lib.touch(entry("ccc333", "gamma", "2026-07-10T10:00:00Z")));
    CHECK(lib.count() == 3);

    // Most recently opened first (ISO-8601 sorts lexically).
    const auto es = lib.entries();
    REQUIRE(es.size() == 3);
    CHECK(es[0].title == "beta");
    CHECK(es[1].title == "gamma");
    CHECK(es[2].title == "alpha");

    // Re-touching an existing hash updates in place, no duplicate.
    auto e = entry("aaa111", "alpha", "2026-07-20T10:00:00Z");
    e.placed = 12;
    e.hasGolden = true;
    REQUIRE(lib.touch(e));
    CHECK(lib.count() == 3);
    const auto es2 = lib.entries();
    CHECK(es2[0].title == "alpha");
    CHECK(es2[0].placed == 12);
    CHECK(es2[0].hasGolden);

    // Empty hash refused (unhashable board must not pollute the registry).
    CHECK(!lib.touch(entry("", "nohash", "2026-07-21T00:00:00Z")));
    CHECK(lib.count() == 3);
}

TEST_CASE("BoardLibrary — persists across a reopen; remove works", "[library]")
{
    TempDir tmp;
    const fs::path file = tmp.path / "board_library.json";
    {
        BoardLibrary lib;
        REQUIRE(lib.open(file));
        REQUIRE(lib.touch(entry("aaa111", "alpha", "2026-07-01T10:00:00Z")));
        REQUIRE(lib.touch(entry("bbb222", "beta",  "2026-07-02T10:00:00Z")));
    }
    BoardLibrary reopened;
    REQUIRE(reopened.open(file));
    CHECK(reopened.count() == 2);
    CHECK(reopened.entries()[0].title == "beta");

    REQUIRE(reopened.remove("bbb222"));
    CHECK(!reopened.remove("bbb222"));   // already gone
    CHECK(reopened.count() == 1);

    BoardLibrary again;
    REQUIRE(again.open(file));
    CHECK(again.count() == 1);
    CHECK(again.entries()[0].title == "alpha");
}

TEST_CASE("BoardLibrary — corrupt file starts empty without throwing", "[library]")
{
    TempDir tmp;
    const fs::path file = tmp.path / "board_library.json";
    { std::ofstream(file) << "[[["; }

    BoardLibrary lib;
    CHECK(!lib.open(file));
    CHECK(lib.count() == 0);
    REQUIRE(lib.touch(entry("ddd444", "delta", "2026-07-18T00:00:00Z")));
    BoardLibrary reopened;
    REQUIRE(reopened.open(file));
    CHECK(reopened.count() == 1);
}
