#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"

using namespace ibom::ai;

TEST_CASE("InferenceEngine — construction", "[ai][inference]")
{
    ModelManager manager("nonexistent_models_dir");
    InferenceEngine engine(manager);
    REQUIRE_FALSE(engine.isReady());
}

TEST_CASE("InferenceEngine — initialization CPU", "[ai][inference]")
{
    ModelManager manager("nonexistent_models_dir");
    InferenceEngine engine(manager);
    bool ok = engine.initialize(false, 0); // CPU only
    REQUIRE(ok);
}

TEST_CASE("InferenceEngine — load nonexistent model", "[ai][inference]")
{
    ModelManager manager("nonexistent_models_dir");
    InferenceEngine engine(manager);
    engine.initialize(false, 0);

    bool loaded = engine.loadModel("nonexistent_model.onnx");
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(engine.isReady());
}

TEST_CASE("InferenceEngine — detect on empty image", "[ai][inference]")
{
    ModelManager manager("nonexistent_models_dir");
    InferenceEngine engine(manager);
    engine.initialize(false, 0);

    cv::Mat empty;
    auto results = engine.detect(empty);
    REQUIRE(results.empty());
}

TEST_CASE("ModelManager — empty models directory", "[ai][models]")
{
    ModelManager manager("nonexistent_models_dir");
    manager.scanModels();

    REQUIRE(manager.availableModels().empty());
}

TEST_CASE("ModelManager — class name for unknown id", "[ai][models]")
{
    ModelManager manager("nonexistent_models_dir");

    // Unknown class id returns a fallback "class_<id>" string
    auto name = manager.className(-1);
    REQUIRE(name == "class_-1");
}
