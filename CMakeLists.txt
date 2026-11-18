# CMakeLists.txt - mingw-w64 / MSVC 共通 (Visual Studio 2026 では OtonumaASIO.sln を使ってください)
cmake_minimum_required(VERSION 3.16)
project(OtonumaASIO CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(BSL_SUFFIX 64)
else()
  set(BSL_SUFFIX 32)
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/utf-8 /W3 /MP)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wno-unknown-pragmas -Wno-missing-field-initializers -O2)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(COMMON_SRC
  src/common/json_mini.cpp
  src/common/settings.cpp
  src/engine/util.cpp)
set(BACKEND_SRC
  src/backend/backend_common.cpp
  src/backend/be_null.cpp
  src/backend/be_wasapi.cpp
  src/backend/be_ks.cpp
  src/backend/be_asio.cpp)
set(SYS_LIBS ksuser setupapi avrt ole32 oleaut32 uuid winmm shell32 advapi32 user32)

add_library(OtonumaASIO SHARED
  ${COMMON_SRC} ${BACKEND_SRC}
  src/engine/engine.cpp
  src/front/asio_driver.cpp
  src/front/OtonumaASIO.def)
set_target_properties(OtonumaASIO PROPERTIES OUTPUT_NAME OtonumaASIO${BSL_SUFFIX} PREFIX "")
target_link_libraries(OtonumaASIO ${SYS_LIBS})
if(NOT MSVC)
  target_link_options(OtonumaASIO PRIVATE -Wl,--kill-at -Wl,--enable-stdcall-fixup)
endif()

add_executable(OtonumaPanel WIN32 ${COMMON_SRC} ${BACKEND_SRC} panel/OtonumaPanel.cpp)
target_link_libraries(OtonumaPanel ${SYS_LIBS} comctl32 gdi32 comdlg32)
if(NOT MSVC)
  target_link_options(OtonumaPanel PRIVATE -municode)
endif()

add_executable(otonumareg tools/otonumareg.cpp)
target_link_libraries(otonumareg advapi32 shell32 user32)
if(NOT MSVC)
  target_link_options(otonumareg PRIVATE -municode)
endif()

add_executable(asiohosttest tools/asiohosttest.cpp)
target_link_libraries(asiohosttest ole32 advapi32 user32)
