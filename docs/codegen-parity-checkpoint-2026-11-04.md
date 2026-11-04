# 代码生成收口检查点 · 2026-11-04 · Batch37

固定原 Bionic/CoreMark 输入上，Block 与 Function **分别通过总编译去纯 byte-decode ≤2× FEX，以及运行耗时、user cycles、user instructions ≤1.10× FEX**。这是本输入、本机配置和已实现路径的检查点，不代表完整 ISA、全部应用或完整信号/线程支持。[原综合收据](evidence/parity-batch37/measurement-receipt.json)与[证据索引](evidence/parity-batch37/README.md)随提交保存。

| 指标 | 固定 FEX | Block（相对 FEX） | Function（相对 FEX） |
| --- | ---: | ---: | ---: |
| 总 compile 去纯 byte，3 次中位数 ms | 47.3196875 | 48.187132（1.018332×） | 84.762869（1.791281×） |
| 有效 100000 迭代耗时 ms | 15072 | 16302（1.081608×） | 15493（1.027933×） |
| whole-child user cycles | 17,324,067,619 | 18,728,836,155（1.081088×） | 17,846,732,735（1.030170×） |
| whole-child user instructions | 61,981,552,180 | 63,676,531,817（1.027347×） | 61,145,976,904（0.986519×） |

三模式长运行 exit0、CRC `e714/1fd7/8e3a/d340` 相同且时长有效；编译另取各 3 次 fresh 20 迭代，短运行只供编译时间/CRC，不作为 CoreMark 分数。只扣纯 byte decoder，未扣形成、IR、优化、分配、发码或发布；A/B 分项不是总预算的替代。Function 成本含 **722 次 Function 尝试及 24 次实际 Block 重试**，不宣称零 fallback。整批收益不归因于单一指令。[两份原摘要](evidence/parity-batch37/compile-summary.json)与[runtime 摘要](evidence/parity-batch37/runtime-summary.json)完整保留；后者历史 `goal_achieved:false` 占位未改，综合判定见收据。

本批三个有界改动：无旗、相邻单用 U32 `Mul→Add` 直接 MADD；无旗 U8 立即数运算的唯一相邻 pinned 字节发布复用 BFXIL 截位；U32 ordered-load→相邻 U64 SignExtend 复用 signed-load 与精确冷热故障恢复。全旗矩阵 Add 保留。四 actual unit 离线核对 IR/Home 不变、选择真实命中；不冒称绑定运行 RX。Host 5/5、Android backend 264579 checks、12+4170 pipeline 路径及 formatter/SMC/EOF 合并场景通过。spill25/42、maxframe48/64B 保留。[离线审阅](evidence/parity-batch37/native-quality-review.md)

测量身份（SHA-256，完整清单见 [inputs](evidence/parity-batch37/inputs.json)）：

- 原 guest：`d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`。
- 普通运行固定 FEXLoader：`8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`；扣 byte 的独立 FEXLoader-byte：`d2e4ce32b6c650bb9f7ce46a91aff8a098431ad3f5f52f52fd746d30b3500995`。
- 候选 runner：`d927b24230bd4e9b80cb771394d8761dd1c382a59a0391f750321d9a9aa051aa`；冻结 80 文件摘要：`79c8bf0c2046ab35ece932b5e74310b13d5de339b00ee01e4def117235cdd985`。

RedMagic `bf1359ff`，真实 child affinity CPU4（`taskset 10`）；policy 前后 min/max 614400–1171200 kHz，current1171200、温度25.4°C，不称锁频。FEX 环境、命令和原 warning 完整保留。仅本轮 namespace 已清零，无活进程、打开文件或清理错误。

复现构建概要（原 build/test 输出在证据包，设备完整命令在 [commands](evidence/parity-batch37/commands.json)；本次提交目录另已完成默认全目标构建与 Host 5/5 检查，未重跑手机测量）：

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
cmake -S . -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static
cmake --build build/android --target svm_rebuild_linux svm_arm64_compile_test svm_compile_pipeline_test -j4
```

使用 NDK29；原 Android 为 Release/O3/DNDEBUG、LTO off。普通运行参数 `0 0 0 100000 7 1 2000`，编译测量改20并启候选 `--decode-timing` / FEX byte probe。

原仓库分支、index、WIP 保持冻结；提交树为独立 worktree `/Volumes/Data2/SwiftVM-submit-20261104`，分支 `refactor/middle-pipeline-rebuild-parity`，基点 `660b6ba1`。原 80 文件 SHA-256 清单绑定测量源码；Git 侧须逐项一致后以实际提交记录标识，新增打包文档不在原测量清单内。该摘要不是 Git commit SHA，此处不预填未来提交号。原始证据本机路径为 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch37`；仓库相对链接指向本次逐字小包，两者区分。

附带的 `benchmarks/app_inputs/build_coremark.py` 是历史 syscall-only CoreMark 配方，保留了原机器 NDK 路径；它不是本次原 Bionic 输入的通用重建脚本。测量输入以原 ELF SHA 为准，不能用另一个重编 ELF 替代后沿用本轮结论。
