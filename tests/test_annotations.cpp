#include <catch2/catch_test_macros.hpp>

#include "features/Annotations.h"

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
             / ("ibom_notes_test_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

} // namespace

TEST_CASE("AnnotationStore — set / read / remove roundtrip", "[notes]")
{
    TempDir tmp;
    const fs::path file = tmp.path / "abc123.json";

    AnnotationStore store;
    REQUIRE(store.open(file));          // missing file → empty store, ok
    CHECK(store.count() == 0);
    CHECK(!store.hasNote("R1"));
    CHECK(store.noteText("R1").empty());

    REQUIRE(store.setNote("R1", "tombstoned on proto 2", "2026-07-18T10:00:00", "front"));
    REQUIRE(store.setNote("U3", "check pin 1 orientation", "2026-07-18T10:01:00", "back"));
    CHECK(store.count() == 2);
    CHECK(store.hasNote("R1"));
    CHECK(store.noteText("R1") == "tombstoned on proto 2");
    CHECK(store.notesFor("U3").size() == 1);
    CHECK(store.notesFor("U3").front().face == "back");
    CHECK(store.annotatedRefs().count("U3") == 1);

    // Replace (V1: single note per ref).
    REQUIRE(store.setNote("R1", "actually fine", "2026-07-18T11:00:00", "front"));
    CHECK(store.notesFor("R1").size() == 1);
    CHECK(store.noteText("R1") == "actually fine");

    // Empty text removes.
    REQUIRE(store.setNote("U3", "", "2026-07-18T11:01:00", "back"));
    CHECK(!store.hasNote("U3"));
    CHECK(store.count() == 1);
}

TEST_CASE("AnnotationStore — persists across a reopen", "[notes]")
{
    TempDir tmp;
    const fs::path file = tmp.path / "board.json";
    {
        AnnotationStore store;
        REQUIRE(store.open(file));
        REQUIRE(store.setNote("C7", "bulged, replace", "2026-07-18T09:00:00", "front"));
    }
    AnnotationStore reopened;
    REQUIRE(reopened.open(file));
    CHECK(reopened.count() == 1);
    CHECK(reopened.noteText("C7") == "bulged, replace");
    CHECK(reopened.notesFor("C7").front().timestamp == "2026-07-18T09:00:00");
}

TEST_CASE("AnnotationStore — corrupt file starts empty without throwing", "[notes]")
{
    TempDir tmp;
    const fs::path file = tmp.path / "broken.json";
    { std::ofstream(file) << "{ not json !!!"; }

    AnnotationStore store;
    CHECK(!store.open(file));           // reported, but usable
    CHECK(store.count() == 0);
    // And it can be written over.
    REQUIRE(store.setNote("R9", "note", "t", "front"));
    AnnotationStore reopened;
    REQUIRE(reopened.open(file));
    CHECK(reopened.noteText("R9") == "note");
}
