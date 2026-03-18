cmake_minimum_required(VERSION 3.28)

find_package(Catch2 3 REQUIRED)

# Helper: add a test target
function(add_ibom_test NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_link_libraries(${NAME} PRIVATE
        Catch2::Catch2WithMain
        opencv_core
        opencv_imgproc
        nlohmann_json::nlohmann_json
    )
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

add_ibom_test(test_ibom_parser       test_ibom_parser.cpp)
add_ibom_test(test_homography        test_homography.cpp)
add_ibom_test(test_inference         test_inference.cpp)
add_ibom_test(test_component_matching test_component_matching.cpp)
