cmake_minimum_required(VERSION 3.20)
project(smb3pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The user's own ROM. Never committed; the translator verifies its bytes.
set(SMB3_ROM "${CMAKE_SOURCE_DIR}/Super Mario Bros. 3 (U) (PRG1) [!].nes" CACHE FILEPATH
    "Path to the user's legally obtained SMB3 (U) PRG1 ROM")
# Vendored disassembly subset (smb3.asm + the PRG sources it includes; textures,
# editor files and binaries stripped). From captainsouthbird/smb3 at 09b1bd8.
set(SMB3_SRC "${CMAKE_SOURCE_DIR}/tools/translate/smb3-src/smb3.asm" CACHE FILEPATH
    "Path to smb3.asm from the vendored SMB3 disassembly subset")
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(ASSET_DIR "${CMAKE_SOURCE_DIR}/assets")

# 1) Extract PRG/CHR from the ROM into assets/ (gitignored). Ran at build time.
add_custom_command(
    OUTPUT "${ASSET_DIR}/prg.bin" "${ASSET_DIR}/chr.bin"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/extract/extract.py"
            --rom "${SMB3_ROM}" --out "${ASSET_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/extract/extract.py"
    COMMENT "Extracting PRG/CHR from the user's ROM"
    VERBATIM)
add_custom_target(smb3_assets DEPENDS "${ASSET_DIR}/prg.bin" "${ASSET_DIR}/chr.bin")

# 2) Translate the disassembly to C++ (verified byte-for-byte against the ROM).
file(GLOB TRANSLATOR_INPUTS "${CMAKE_SOURCE_DIR}/tools/translate/*.py")
# The translator writes bank_00..bank_31.cpp (empty where a bank has no code)
# and bank_table.inc. All 32 are listed here so nothing depends on configure-time globbing.
set(GENERATED_BANKS)
foreach(n RANGE 0 31)
    if(n LESS 10)
        set(id 0${n})
    else()
        set(id ${n})
    endif()
    list(APPEND GENERATED_BANKS "${GEN_DIR}/bank_${id}.cpp")
endforeach()
add_custom_command(
    OUTPUT ${GENERATED_BANKS} "${GEN_DIR}/bank_table.inc"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/translate/translate.py"
            --src "${SMB3_SRC}" --rom "${SMB3_ROM}" --out "${GEN_DIR}"
    DEPENDS ${TRANSLATOR_INPUTS} "${SMB3_SRC}"
    COMMENT "Translating 6502 disassembly to C++ (verifying against ROM)"
    VERBATIM)
add_custom_target(smb3_translate DEPENDS ${GENERATED_BANKS} "${GEN_DIR}/bank_table.inc")

# 3) Runtime core and generated code.
# Linear Blargg APU (Nes_Snd_Emu 0.1.7, single Blip_Buffer). Nonlinear_Buffer is
# intentionally not compiled in.
add_library(smb3_core STATIC
    src/runtime/machine.cpp
    src/runtime/ppu.cpp
    src/runtime/apu.cpp
    src/runtime/nes_apu/Blip_Buffer.cpp
    src/runtime/nes_apu/Nes_Apu.cpp
    src/runtime/nes_apu/Nes_Oscs.cpp
    src/runtime/nes_apu/apu_snapshot.cpp
    ${GENERATED_BANKS}
)
target_include_directories(smb3_core PUBLIC src ${GEN_DIR})
target_compile_options(smb3_core PRIVATE -Wall -Wno-unused-label)
set_source_files_properties(${GENERATED_BANKS} PROPERTIES COMPILE_OPTIONS "-O1")
add_dependencies(smb3_core smb3_translate smb3_assets)

# 4) Headless runner for verification.
add_executable(smb3_headless src/tools/headless.cpp)
target_link_libraries(smb3_headless PRIVATE smb3_core)

# 5) Desktop app: SDL3 window + Vulkan presenter. Optional: needs SDL3 and Vulkan.
find_package(Vulkan)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
endif()
find_program(GLSLC glslc)

if(Vulkan_FOUND AND SDL3_FOUND AND GLSLC)
    set(SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
    set(SHADER_SPVS)
    foreach(stage vert frag)
        set(src "${CMAKE_SOURCE_DIR}/src/app/shaders/present.${stage}.glsl")
        set(spv "${SHADER_DIR}/present.${stage}.spv")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
            COMMAND "${GLSLC}" -fshader-stage=${stage} "${src}" -o "${spv}"
            DEPENDS "${src}"
            COMMENT "Compiling present.${stage} shader"
            VERBATIM)
        list(APPEND SHADER_SPVS "${spv}")
    endforeach()
    add_custom_target(smb3_shaders DEPENDS ${SHADER_SPVS})

    add_executable(smb3pc src/app/main.cpp src/app/presenter.cpp src/app/overlay.cpp)
    target_link_libraries(smb3pc PRIVATE smb3_core PkgConfig::SDL3 Vulkan::Vulkan)
    target_compile_definitions(smb3pc PRIVATE
        SMB3_ASSET_DIR="${ASSET_DIR}"
        SMB3_SHADER_DIR="${SHADER_DIR}")
    target_compile_options(smb3pc PRIVATE -Wall)
    add_dependencies(smb3pc smb3_shaders smb3_assets)
else()
    message(STATUS "smb3pc desktop app skipped (needs SDL3, Vulkan and glslc)")
endif()
