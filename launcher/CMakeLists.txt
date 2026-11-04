# One program to start either game. Standalone: it links nothing of the games', so it builds in seconds, and it sits
# next to soh.exe.
add_executable(HarkinianVR WIN32 main.cpp Library.cpp Library.h Zip.cpp Zip.h)
target_compile_features(HarkinianVR PRIVATE cxx_std_20)
target_compile_definitions(HarkinianVR PRIVATE UNICODE _UNICODE)
target_link_libraries(HarkinianVR PRIVATE gdiplus shlwapi shell32 ole32 uuid)
set_target_properties(HarkinianVR PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
