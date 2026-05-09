#include <catch2/catch_test_macros.hpp>

#include "camera/UnifiedAllocator.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

using namespace ibom::camera;

TEST_CASE("UnifiedAllocator — singleton is non-null", "[camera][unified-allocator]")
{
    cv::MatAllocator* a = unifiedAllocator();
    REQUIRE(a != nullptr);
    REQUIRE(unifiedAllocator() == a); // stable across calls
}

TEST_CASE("UnifiedAllocator — basic Mat allocation through custom allocator",
          "[camera][unified-allocator]")
{
    cv::Mat m;
    m.allocator = unifiedAllocator();
    m.create(720, 1280, CV_8UC3);

    REQUIRE_FALSE(m.empty());
    REQUIRE(m.rows == 720);
    REQUIRE(m.cols == 1280);
    REQUIRE(m.type() == CV_8UC3);
    REQUIRE(m.data != nullptr);
    REQUIRE(m.isContinuous());
}

TEST_CASE("UnifiedAllocator — write then read pixels round-trips correctly",
          "[camera][unified-allocator]")
{
    cv::Mat m;
    m.allocator = unifiedAllocator();
    m.create(64, 64, CV_8UC3);

    m.setTo(cv::Scalar(10, 20, 30));

    auto px = m.at<cv::Vec3b>(32, 32);
    REQUIRE(px[0] == 10);
    REQUIRE(px[1] == 20);
    REQUIRE(px[2] == 30);

    // Mutate and re-read
    m.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 128, 64);
    auto px2 = m.at<cv::Vec3b>(0, 0);
    REQUIRE(px2[0] == 255);
    REQUIRE(px2[1] == 128);
    REQUIRE(px2[2] == 64);
}

TEST_CASE("UnifiedAllocator — buffer survives Mat copy/release cycles",
          "[camera][unified-allocator]")
{
    cv::Mat src;
    src.allocator = unifiedAllocator();
    src.create(128, 128, CV_8UC1);
    src.setTo(cv::Scalar(42));

    // Shallow copy shares the buffer; deep copy must produce an independent
    // valid Mat (using the default allocator for the destination is fine).
    cv::Mat shallow = src;
    REQUIRE(shallow.data == src.data);

    cv::Mat deep;
    src.copyTo(deep);
    REQUIRE(deep.data != src.data);
    REQUIRE(deep.at<uchar>(64, 64) == 42);

    // Release source — shallow copy must remain valid (refcount).
    src.release();
    REQUIRE(shallow.at<uchar>(64, 64) == 42);
}

TEST_CASE("UnifiedAllocator — works with OpenCV imgproc operations in-place",
          "[camera][unified-allocator]")
{
    cv::Mat m;
    m.allocator = unifiedAllocator();
    m.create(100, 100, CV_8UC3);
    m.setTo(cv::Scalar(0, 255, 0));

    cv::Mat gray;
    gray.allocator = unifiedAllocator();
    cv::cvtColor(m, gray, cv::COLOR_BGR2GRAY);

    REQUIRE(gray.rows == 100);
    REQUIRE(gray.cols == 100);
    REQUIRE(gray.type() == CV_8UC1);
    // BGR (0,255,0) → gray ≈ 150 (0.587 * 255)
    REQUIRE(gray.at<uchar>(50, 50) > 140);
    REQUIRE(gray.at<uchar>(50, 50) < 160);
}

TEST_CASE("UnifiedAllocator — unifiedMemoryAvailable matches build flag",
          "[camera][unified-allocator]")
{
    bool avail = unifiedMemoryAvailable();
#ifdef IBOM_USE_UMA_ALLOCATOR
    // On a CUDA-capable host the probe should succeed; on a host without a
    // GPU the allocator falls back transparently and reports false. Either
    // outcome is acceptable — we just check the predicate is callable.
    INFO("IBOM_USE_UMA_ALLOCATOR defined; runtime probe says: "
         << (avail ? "available" : "fallback"));
#else
    REQUIRE_FALSE(avail);
#endif
}
