# 上游接入：flac（CMake）。交叉时上游已由 scripts/local-cross.sh 编到 PKG_PREFIX，
# 探针只链接静态库 libFLAC.a，不生成 pkg 目标。

add_executable(flac_harness test/pkg_flac.c)
target_include_directories(flac_harness PRIVATE "${PKG_PREFIX}/include")
target_link_directories(flac_harness PRIVATE "${PKG_PREFIX}/lib")
target_link_libraries(flac_harness PRIVATE FLAC m)
target_compile_options(flac_harness PRIVATE ${RVV_FLAGS})

add_test(NAME flac_harness COMMAND flac_harness)
