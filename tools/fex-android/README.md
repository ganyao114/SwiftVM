# Android FEX 固定源码参照

此目录保存红魔 NX729J（`bf1359ff`）对照所用的 Android 构建配方与平台补丁。
源码固定为 [BulkanDrivers/FEX-android](https://github.com/BulkanDrivers/FEX-android)
的 `a156752fae3a4f65fefb1ed95af980bcdfac43d7`，提交日期为 2025 年 8 月 23 日。
它与[重构方案](../../docs/middle-pipeline-rebuild-plan-2026-10-21.md)第 2.4 节的
2026 年官方设计参考不同；比较结果应使用此处的实际提交及补丁身份。

## 构建

已使用 macOS/arm64、CMake 4.2.3、Ninja、Python 3 和 NDK
`29.0.13846066-beta3`（Clang 21）构建。NDK 目录名为 `29.0.13846066`，
实际版本以其中的 `source.properties` 为准。以下命令从 SwiftVM 根目录开始，
主机源码、构建缓存和临时文件统一保存在 Data2：

```sh
svm_repo=$PWD
fex_cache=/Volumes/Data2/swiftvm-middle-build-cache/fex-android
fex_src="$fex_cache/source"
fex_build="$fex_cache/build"
fex_ndk=/Users/swift/Library/Android/sdk/ndk/29.0.13846066
export TMPDIR=/Volumes/Data2/swiftvm-middle-build-cache/tmp-native
mkdir -p "$fex_cache" "$TMPDIR"
```

仅在固定源码缓存尚不存在时执行初始化：

```sh
git init "$fex_src"
git -C "$fex_src" remote add origin https://github.com/BulkanDrivers/FEX-android.git
git -C "$fex_src" fetch --depth 1 origin a156752fae3a4f65fefb1ed95af980bcdfac43d7
git -C "$fex_src" checkout --detach FETCH_HEAD
git -C "$fex_src" submodule update --init --depth 1 -- \
  External/fmt External/xxhash External/vixl External/robin-map \
  External/drm-headers Source/Common/cpp-optparse
git -C "$fex_src" apply --check "$svm_repo/tools/fex-android/android-ndk-r29.patch"
git -C "$fex_src" apply "$svm_repo/tools/fex-android/android-ndk-r29.patch"
```

已有缓存须先核对固定提交、平台补丁及配置身份，然后复用该源码和构建目录；
不要重复应用补丁或另建整套 clone/build。首次配置或构建参数变化时运行 configure，
后续使用同一构建目录增量构建：

```sh
cmake -S "$fex_src" -B "$fex_build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$fex_ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_TESTS=OFF -DBUILD_TESTING=OFF -DBUILD_FEX_LINUX_TESTS=OFF \
  -DBUILD_FEXCONFIG=OFF -DBUILD_THUNKS=OFF \
  -DENABLE_JEMALLOC=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF \
  -DENABLE_GDB_SYMBOLS=OFF -DENABLE_OFFLINE_TELEMETRY=OFF \
  -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF -DENABLE_CCACHE=OFF \
  -DTUNE_CPU=none \
  -DCMAKE_INSTALL_PREFIX=/data/local/tmp/svm-middle-fex-fixed-a156
cmake --build "$fex_build" --target FEXLoader FEXServer -j 6
```

仅在整个两目标构建退出零后，取 `Bin/FEXLoader` 与 `Bin/FEXServer`。
两者使用静态 libc++，所需动态库为 Android 系统的 libc/libm/libdl。
此配置用于当前 64 位静态 x86_64 guest；jemalloc 和 thunks 均关闭，
不据此声称已支持 32 位 guest 或 native thunks。

保存本次完整命令、工具版本、`git rev-parse HEAD`、`git diff --binary`、
`git submodule status`、配置/构建日志及实际 ELF 的 SHA-256。
补丁让无 tag 的源码也有 `git describe --always --dirty` 标识；
`a156752-dirty` 表示该提交带本地补丁，具体改动仍以 patch 哈希为准。
重建后的产物必须按实际哈希登记，不能沿用另一轮二进制的身份。
历史 `/private/tmp` 入口仅作兼容软链；新结果与大量临时数据直接写 Data2，
结果校验、保留唯一证据后及时回收可再生成的重复产物。

## 补丁范围

[android-ndk-r29.patch](android-ndk-r29.patch) 适配 Bionic/libc++ 的头文件、
回调存储、信号掩码、ELF 类型、应用数据目录下的统计文件和 Android 链接设置。
服务目录初始化按错误码和目录实际状态判定，处理带末尾 `/` 的新目录已创建、
但 `create_directories` 返回 false 的首次启动路径，并允许并发创建后的正常复用。
捕获回调仍由 FEX allocator 持有；Android wrapper 增加一个调用 thunk 指针。
Android arm64 的 x18 为 ShadowCallStack 保留，补丁将其从 x64 动态分配及保存集移除，
动态 GPR 数由七个降为六个；这是会影响生成代码的 ABI 约束变化。
依据为 [NDK ABI 文档](https://developer.android.com/ndk/guides/abis#arm64-v8a)。
IR pass 算法没有修改。平台适配的结果必须与其对应 ELF 一起记录。

## 设备运行与比较范围

把两份 ELF 放在同一个独立目录的 `bin/` 下，分别设置
`FEX_APP_DATA_LOCATION`、`FEX_APP_CONFIG_LOCATION`、`XDG_RUNTIME_DIR` 和
`FEX_SERVERSOCKETPATH`，避免共享用户现有服务或缓存。目录末尾的 `/` 保留在
data/config 的环境变量值中。当前静态 guest 使用 `FEX_ROOTFS=/`，
并显式设置 `FEX_AOTIRLOAD=0`、`FEX_AOTIRCAPTURE=0`、`FEX_AOTIRGENERATE=0`、
`FEX_CACHEOBJECTCODECOMPILATION=0`。本轮使用 `FEX_HOSTFEATURES=disableavx`，
对齐 SVM 缺省关闭的 AVX/XSAVE/BMI 相关路径；ADX、FSGSBASE 等其他 CPUID 差异
仍须核对 guest 实际分派，不能据此宣称全部 guest 可见能力相同。
配置文件、ELF、guest 均在运行前后核对哈希。

初版产物在 launcher 自动启动服务时连接失败；预启动前台服务后曾通过
CoreMark 2000 和 bench。新目录复现将问题定位到上述目录创建判定。
当前补丁已在完整重建后验证：服务目录原先不存在时，launcher 能自动拉起服务；
随后复用已有目录，CoreMark 2000/12000 的 CRC 分别为 `0x4983`/`0xd340`，
bench 五项 checksum 全部匹配。服务路径为普通文件时退出 255，报告
`Not a directory`；失败拒绝保留。前台服务的超时诊断单独保存。
CoreMark 的短迭代会报告运行不足十秒，该结果只用于此处的固定工作量与 CRC 核对，
不发布为符合 CoreMark 规则的跑分。

按重构方案 14.6/15.5，正式 PMU 应明确预启动服务的状态与统计范围：
若 FEXServer 在被采集进程树外，其启动和工作成本单列；
端到端启动成本另设包含服务的采集范围。翻译器新进程与服务预热分别记录，
退出时仅通过相同私有环境执行 `FEXServer -k` 并等待自己的服务结束。
本目录的构建成功及初步 CRC/checksum 不代表正式三方 PMU 或完整 R0 通过。
