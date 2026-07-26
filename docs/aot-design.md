# AOT 预编译与 host→guest 调用层：设计

状态：设计已定，实现未开始。本文是实现的任务书，也是设计决策的记录。

---

## 0. 目标与形态

把一个 x86-64 Linux ELF **离线**编译成 ARM64 代码，产物是一个**合法的 ELF**：
保留原 ELF 的结构与符号，代码段重填为编译后的 ARM64 代码，符号表随之修正。
之后 host 程序可以按符号名直接调用其中的 guest 函数，参数按 x86-64 SysV ABI
由模板静态解析后传递。

这两件事是一体的：符号表之所以要修，正是因为要让 host 能按名字找到函数；
代码段之所以要重填，正是因为要让找到的东西可以直接执行。

---

## 1. 产物形态：为什么是 AArch64 ELF，以及一个必须保留的约束

**产物 `e_machine = EM_AARCH64`，`ET_DYN`（共享对象）。**

理由：如果只是在原 x86-64 ELF 上挂一个附加节存放编译结果，符号表就不需要修
——符号仍指向原来的 x86 代码。任务要求「重新填充编译后的代码段，修复符号表」，
只有当符号最终指向**可执行的 host 代码**时才成立，任务四（host 按符号调用）
也只有这样才有意义。

**但有一个不能动的约束：guest 数据必须留在原来的 guest 虚拟地址上。**

编译出来的代码访问 guest 内存的方式是 `host = (guest & mask) + bias`（见
`docs/project-status.md` 的 P-1 条目与提交 `81fa5d6`）。任何一条访存指令里
编码的都是 guest 地址。所以：

| 内容 | 在产物 ELF 里的位置 | 地址 |
|---|---|---|
| `.data` / `.rodata` / `.bss` / 原 `.text` 的只读引用 | 原样拷贝 | **保持原 guest 虚拟地址** |
| 编译出的 ARM64 代码 | 新的可执行节 | host 地址，与 guest 地址空间无关 |

产物因此是个混合体：**guest 数据在 guest 地址上，host 代码在 host 地址上**。
加载器必须把数据段映射进 guest 窗口（`SVM_GUEST_BITS` 那块保留区）内的原始
地址，把代码段按普通共享对象映射。这一条如果做错，表现是「大部分能跑、访问
全局变量时读到垃圾」——**必须有一个专门断言全局变量读写正确的用例**。

**符号表**：`.symtab`/`.dynsym` 的 `STT_FUNC` 条目，`st_value` 改为新代码的
host 偏移、`st_size` 改为编译后长度、`st_shndx` 指向新代码节；`STT_OBJECT`
条目**不动**（数据没搬家）。原符号名一律保留——这是任务的明确要求，也是任务
四的前提。无法编译的函数保留符号但指向一个**响亮失败的桩**，不要静默留下
x86 字节。

---

## 2. 产物不是自足的

编译出来的代码需要运行时提供：`ThreadContext64*`（state 寄存器）、guest 内存
bias 与掩码、L2 分派表、trampoline（间接跳转/返回宿主）、以及一批 host helper
的绝对地址（x87 SoftFloat、`rep` 系列、xsave、`pcmpXstrY` 求值器……）。

所以产物是**共享对象**，导出一个初始化入口：

```c
// 由 host 在 dlopen 之后、任何 guest 调用之前调用一次。
// 失败返回非零，host 必须放弃使用该产物（而不是继续跑）。
int swift_aot_init(const SwiftAotRuntime* rt);
```

`SwiftAotRuntime` 里放上述指针。**版本与配置校验也在这里做**（见 §4）。

---

## 3. 函数发现与编译粒度

**主路径：`.symtab` 的 `STT_FUNC` 条目**给出函数起止，这是 AOT 相对 JIT 的
根本优势——JIT 只能在运行时从调用目标发现函数，AOT 有完整的符号表。

- 无符号（stripped）的二进制：退化为从入口点做 CFG 发现。**如实记为能力缺口**，
  不要假装覆盖。
- 符号大小为 0、或多个符号指向同一地址（别名、`weak`）：需要明确策略并写进报告。
- 函数内部有间接跳转表（switch）：目标集合静态不可知，落到分派器。**这是正确性
  边界，不是性能问题**——漏掉一个目标就是跳飞。

编译复用现有函数级路径（`backend/runtime.cpp` 的 `TranslateIR(module, HIRFunction*)`
及其上游），不要另起一套前端。**任何「AOT 专用的翻译分支」都会与 JIT 分岔，
而分岔处的语义差异是查不出来的那种 bug。**

---

## 4. 有效性校验：产物必须知道自己配给谁

与 JIT 磁盘缓存共用同一套校验量（见磁盘缓存那项工作的报告），至少包含：

- 原 guest ELF 的内容哈希（至少覆盖被编译的代码范围）
- 所有影响发射结果的开关：`SVM_FUNC_BASE`、`SVM_STATIC_REGS`、`SVM_UNIFORM_ELIM`、
  `SVM_UNIFORM_DSE`、`SVM_CONST_CSE`、`SVM_FLAG_NARROW`、`SVM_AVX`/`SVM_BMI`/
  `SVM_XSAVE`/`SVM_SSE4`/`SVM_SSE42STR`、`SVM_X87_JIT`/`SVM_X87_TOPVIRT`、
  TSO 模式、`SVM_GUEST_BITS`
- SwiftVM 自身的构建标识

**漏掉任何一项都会让产物在语义不同的配置下被当成有效**——这是本项目最忌讳的
静默错误类别。校验不过必须**拒绝加载**（返回非零），而不是「尽力而为」。

---

## 5. 重定位

复用 JIT 磁盘缓存那项工作产出的序列化/重定位组件。ARM64 上必须覆盖的类别：

- `adrp/add` 对（±4 GiB，页对齐语义）
- 字面量池（`ldr` 的 PC 相对形式，±1 MB）
- `b`/`bl` 的 ±128 MB —— **guest 函数之间的直接调用在大二进制上会超范围**，
  需要桩或改走间接
- 块链接桩（`BlockLinkStub`）与 L2 分派表索引
- host helper 的绝对地址
- RSB 相关的槽位

**枚举不全就是加载后跳飞。** 组件应当以「发射时登记」而非「事后扫描」的方式
收集重定位项——事后扫描无法区分「一条 `bl`」与「一个恰好长得像 `bl` 的常量」。

---

## 6. 调用层（任务四）

目标形态：

```cpp
// 静态解析 Ret(Args...) 的 x86-64 SysV 分类，marshal 进 guest 上下文，
// 运行，再把返回值取回来。
template <typename Sig> class GuestFn;

template <typename Ret, typename... Args>
class GuestFn<Ret(Args...)> {
public:
    explicit GuestFn(void* entry);
    Ret operator()(Args... args) const;
};

auto strlen_g = aot.Lookup<size_t(const char*)>("strlen");
size_t n = strlen_g(guest_ptr);
```

**ABI 分类必须在编译期完成**，这是任务的明确要求（「通过可变参数模板，静态解析
Caller 符号函数的 ABI」）。需要一个 `AbiClass<T>` 元函数按 x86-64 SysV 给出
INTEGER / SSE / MEMORY，然后按序分配：

- INTEGER：RDI, RSI, RDX, RCX, R8, R9，溢出入栈
- SSE：XMM0–XMM7，溢出入栈
- MEMORY（大结构体、非平凡类型）：整体入栈；**返回值为 MEMORY 时 RDI 是隐藏的
  返回缓冲区指针，其余整型参数整体右移一位**——这一条最容易漏
- 变参函数：AL = 使用的向量寄存器个数
- 栈必须在 `call` 前 16 字节对齐（即进入被调函数时 RSP ≡ 8 mod 16）

**指针参数的所有权是设计里最尖锐的一处**：host 传进来的 `const char*` 是 host
地址，而 guest 代码只能理解 guest 地址。两种做法必须明确选一个并写进接口：
1. 只接受**已经在 guest 窗口内**的地址（类型上用一个 `GuestPtr<T>` 区分，误用
   在编译期就失败）；
2. 由调用层负责把数据拷进 guest 内存并回拷。

**推荐 1**：拷贝语义会让「谁拥有这块内存」「回拷时机」变成运行期约定，而
`GuestPtr<T>` 把它变成编译期错误。方案 2 可以作为一个显式的辅助设施
（`ScopedGuestBuffer`）叠在上面，而不是默认行为。

**返回路径**：guest 函数用 `ret` 结束，需要一个能让 `Run()` 停下来的返回地址。
做法是把一个特殊的哨兵地址压栈作为返回地址，`Run()` 遇到它就停——`ExitReason`
要能区分「正常返回」与「guest 崩了」，**后者绝不能被当成返回值读**。

---

## 7. 验证要求（这一项的成败取决于此）

1. **逐字节等价**：同一个 guest 程序，JIT 直接跑 vs AOT 产物跑，**输出与退出码
   必须逐字节一致**。25 个 e2e guest 程序全测。
2. **全局变量正确性**：专门的用例断言 guest 全局数据的读写正确——§1 那个「数据
   留在 guest 地址」的约束做错时，症状恰好是「大部分正常」。
3. **符号表可用性**：产物用 `readelf`/`llvm-readelf` 解析无错；`nm` 能列出原符号；
   `dlopen` + `dlsym` 能取到函数指针。
4. **调用层**：覆盖整型/浮点/混合/大结构体/MEMORY 返回值/变参各至少一例，并与
   同一函数在 JIT 下的行为对拍。
5. **拒绝加载路径**：改一个字节的 guest 镜像、改一个参与哈希的开关、损坏产物
   ——三种都必须**拒绝**而不是尽力而为。
6. **变异测试**：本项目的硬要求。至少证明「某类重定位没回填」「ABI 分类错一位」
   「MEMORY 返回值没有右移整型参数」会被抓住。

---

## 8. 明确的非目标

- 不追求覆盖 stripped 二进制（无符号表时退化，如实记为缺口）
- 不追求自修改代码的 guest（AOT 与 SMC 语义冲突；产物应当**检测到**目标区间被
  写就拒绝继续，而不是执行陈旧代码）
- 不追求动态链接的 guest（先做静态链接的）

---

## 9. 实测的语料现状（2026-07-27，省去重新发现的一轮）

| guest ELF | 节表 | `STT_FUNC` | 带 size 的 | 说明 |
|---|---|---|---|---|
| `func_tests_x86_64` | 28 节，有 `.symtab`/`.strtab` | **1317** | 1310 | 静态 glibc，AOT 的主目标 |
| `real_busy_x86_64` | 有 `.symtab` | **1364** | 1357 | 同上 |
| `hello_x86_64`、`bench_suite_x86_64` 等 | **节表为空（`e_shnum=0`）** | — | — | `mklinuxelf.py` 只写程序头 |

两条结论：

1. **真实目标规模约 1300 个函数**，其中约 0.5% 的 `STT_FUNC` 没有 size（例如
   `__libc_message.cold` 这类），需要一个明确策略——用下一个符号的地址推边界，
   还是跳过并记为缺口。**不要静默按 0 长度处理。**
2. `source/translator/linux/tests/` 下手工构造的 freestanding guest **完全没有
   符号表**，AOT 对它们只能走 CFG 发现。所以**验证语料必须以 `func_tests` /
   `real_busy` 为主**——拿 `hello_x86_64` 做 AOT 的端到端验证会让"符号保留"
   与"符号表修复"这两条要求根本没被测到。

本机没有 `readelf` / `llvm-readelf`，上表是用 Python 直接解析 ELF 结构得到的；
验证脚本不要依赖这两个工具存在。
