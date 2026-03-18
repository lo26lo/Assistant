# CompilerFlags.cmake
# Common compiler flags for MicroscopeIBOM

if(MSVC)
    # MSVC flags
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4             # Warning level 4
        /utf-8          # UTF-8 source and execution charset
        /MP             # Multi-processor compilation
        /permissive-    # Strict standard conformance
        /Zc:__cplusplus # Correct __cplusplus macro
    )

    # Release optimizations
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${PROJECT_NAME} PRIVATE /O2 /GL /Oi)
        target_link_options(${PROJECT_NAME} PRIVATE /LTCG)
    endif()

    # Disable annoying MSVC warnings
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
        NOMINMAX                # Prevent windows.h min/max macros
        WIN32_LEAN_AND_MEAN     # Reduce windows.h includes
    )

elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC / Clang flags
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )

    # Release optimizations
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${PROJECT_NAME} PRIVATE -O3 -march=native -flto)
        target_link_options(${PROJECT_NAME} PRIVATE -flto)
    endif()

    # Debug flags
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${PROJECT_NAME} PRIVATE -g -O0)
    endif()
endif()

# AddressSanitizer
if(IBOM_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address)
endif()
