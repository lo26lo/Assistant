#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"

using namespace ibom::ai;

TEST_CASE("InferenceEngine — construction", "[ai][inference]")
{
    InferenceEngine engine;
    REQUIRE_FALSE(engine.isModelLoaded());
}

TEST_CASE("InferenceEngine — initialization", "[ai][inference]")
{
    InferenceEngine engine;
    bool ok = engine.initialize(false, false); // CPU only
    REQUIRE(ok);
}

TEST_CASE("InferenceEngine — load nonexistent model", "[ai][inference]")
{
    InferenceEngine engine;
    engine.initialize(false, false);

    bool loaded = engine.loadModel("nonexistent_model.onnx");
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(engine.isModelLoaded());
}

TEST_CASE("InferenceEngine — detect on empty image", "[ai][inference]")
{
    InferenceEngine engine;
    engine.initialize(false, false);

    cv::Mat empty;
    auto results = engine.detect(empty);
    REQUIRE(results.empty());
}

TEST_CASE("ModelManager — empty models directory", "[ai][models]")
{
    ModelManager manager;
    manager.setModelsDirectory("nonexistent_models_dir");
    manager.scanModels();

    REQUIRE(manager.availableModels().empty());
}

TEST_CASE("ModelManager — class names", "[ai][models]")
{
    ModelManager manager;

    // Default class names should be empty for unknown model
    auto names = manager.classNames("unknown_model");
    REQUIRE(names.empty());
}
