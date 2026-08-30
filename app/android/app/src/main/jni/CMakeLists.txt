# 从 react-native/ReactAndroid/cmake-utils/default-app-setup/CMakeLists.txt 复制而来，
# 按那份文件顶部的说明扩展。
#
# **不要用 externalNativeBuild.cmake.path 指向别的文件去取代它。** RN 的 gradle
# 插件只在 path 为 null 时才注入这一份，占了它就等于不构建 libappmodules.so——
# 核心 TurboModule 的注册在那里面，app 会以
# 「'PlatformConstants' could not be found」红屏，而报错完全指不到这里。

cmake_minimum_required(VERSION 3.13)

project(appmodules)

include(${REACT_ANDROID_DIR}/cmake-utils/ReactNative-application.cmake)

# ---- 以下是本项目追加的 ----

# 复用 shared/urf 那份跨平台编码器，不复制一份进来——复制出来的那份就再也
# 跑不了 shared/urf 的单测了。
# jni → main → src → app → android → 项目根，五层。
set(URF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared/urf)
if(NOT EXISTS ${URF_DIR}/src/packbits.cpp)
  message(FATAL_ERROR "找不到 shared/urf，实际算出来的路径是 ${URF_DIR}")
endif()

add_library(rasterkit SHARED
  ${CMAKE_CURRENT_SOURCE_DIR}/../cpp/rasterkit_jni.cpp
  ${URF_DIR}/src/packbits.cpp
  ${URF_DIR}/src/writer.cpp
  ${URF_DIR}/src/validate.cpp
)
target_include_directories(rasterkit PRIVATE ${URF_DIR}/include)
target_compile_options(rasterkit PRIVATE -Wall -Wextra)
# jnigraphics 提供 AndroidBitmap_lockPixels，省掉一份和位图等大的缓冲。
target_link_libraries(rasterkit log jnigraphics)
