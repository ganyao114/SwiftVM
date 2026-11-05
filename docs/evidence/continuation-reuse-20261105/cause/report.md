# 重复构造入口定因

输入仅为已闭合的705次冷请求trace和固定CoreMark ELF；没有新运行、构建或生产修改。生产派生快照842e4748…，完整hash在cause.json。

## 直接结论

重复有两条实际入口链，不能只归CALL续点：新入口既包括先前CALL的afterPC，也包括__vfprintf两个间接跳表的目标；这些入口重新形成后又走回先前的公共子图。cache只登记请求root，既有source B没有独立完整入口，这解释了重建的发生条件。

现trace没有每次dispatch的源PC。以下是“真实冷入口与此前已published指令/只读表的精确关系”，不是完整动态控制流还原；多种已知直接角色和其余未知来源均显式保留。

## 各类已发布入口与重复指令出现次数

| 入口关系 | 发布unit | 重复指令出现 |
|---|---:|---:|
| 此前已编译CALL的afterPC | 436 | 6742 |
| 此前已编译direct CALL的目标 | 197 | 925 |
| 此前外部直接分支/顺序或服务frontier | 30 | 1067 |
| 无以上直接关系：间接或未观察来源 | 32 | 7155 |
| 初始ELF root | 1 | 0 |
| 其余Block fallback | 4 | 0 |

该表按角色合计700个发布unit。436个afterPC匹配包含435个成功Function及1个Block fallback；该fallback不再次列入最后一行。另外一个afterPC匹配也有其他直接角色，原始细分类保留在JSON。5个失败前缀的169条指令/251节点仍单列，不混入成功重复15889。

32个未归直接来源入口中，10个精确匹配此前已编译__vfprintf间接跳表的表项，承载7017次重复；其余22个共138次重复。6742与7017相加是实测入口分组的13759次重复覆盖，不能当作某个未来变更的可删除量。

## source B起点事实

18个完全旧覆盖的新root，18/18精确等于此前published Function的非空原始DecodedBlock起点；0个位于B中间。它们合计1323条指令、3590输入节点。除此之外，没有其它新root匹配旧source B起点；也没有root匹配空B。这些不是IR为外部边生成的exitstub。完整18行、旧owner和原B的pc/end/count均在cause.json。

## __vfprintf的14个大主体

这里的14个unit是实际共同覆盖[0x25ce1e,0x25cf3c)的组；__vfprintf另有短prologue/续点请求，不能把全函数总请求数也叫14。

| seq / entry | 与此前指令的关系 | 此前source B起点 | 本unit指令 / 已覆盖指令 |
|---|---|---|---:|
| 221 / 0x25a40f | CALL 0x25a40a 后（owner 220） | 否 | 1000 / 0 |
| 223 / 0x25c004 | CALL 0x25bfff 后（owner 222） | 否 | 879 / 865 |
| 226 / 0x25d640 | CALL 0x25d63b 后（owner 221） | 否 | 872 / 865 |
| 314 / 0x25b19e | JMP 0x25a7f0 表项 68,73 | 否 | 1017 / 865 |
| 315 / 0x25c1d1 | JMP 0x25c193 表项 2 | 是：owner 314，4条 | 897 / 897 |
| 628 / 0x25aa02 | JMP 0x25a7f0 表项 76 | 否 | 874 / 865 |
| 629 / 0x25b336 | JMP 0x25a7f0 表项 85 | 否 | 1012 / 893 |
| 630 / 0x25b9a3 | conditional_fallthrough 0x25b99d（owner 629） | 否 | 902 / 896 |
| 657 / 0x25b770 | CALL 0x25b76b 后（owner 637） | 否 | 965 / 865 |
| 660 / 0x25c9fa | CALL 0x25c9f5 后（owner 221） | 否 | 869 / 865 |
| 673 / 0x25ab85 | JMP 0x25a7f0 表项 16 | 否 | 868 / 865 |
| 674 / 0x25a923 | JMP 0x25a7f0 表项 17,18,19,20,21,22,23,24,25 | 否 | 898 / 865 |
| 675 / 0x25b5a6 | JMP 0x25a7f0 表项 88 | 否 | 995 / 895 |
| 676 / 0x25c23d | JMP 0x25c193 表项 3 | 否 | 888 / 872 |

这14个unit合计12936条指令出现，其中11373次已有覆盖。5个入口匹配CALL后PC，8个匹配间接表项，1个匹配已知外部直接分支。

### 间接表的实际字节证据

- JMP RAX@0x25a7f0：LEA RCX=[RIP-0x53059]得到0x207790；MOVSXD RAX,[RCX+RAX*4]；ADD RAX,RCX。此前CMP EAX,0x5a/JA限制0..90，selector=字符−0x20。数据表在只读PT_LOAD内。seq314对应索引68/73(d/i)，628为76(l)，629为85(u)，673为16(0)，674为17..25(1..9)，675为88(x)。另两个新短入口222/637也精确匹配该表。
- JMP RAX@0x25c193：LEA RDX得到0x20791c；MOVSXD/ADD组合。只报告已观察入口所对应的精确表项：index2→0x25c1d1(seq315)，index3→0x25c23d(seq676)，不猜整个表的长度。该jump最先在seq314编译；seq315随后出现，且此前源码路径已有MOV EAX,2→JNS到该分支。

因此，seq315并非CALL续点：它不匹配任何此前已编译CALL的afterPC，而是已编译跳表的literal目标；并且旧seq314已含这个source B，但未给它完整可调用入口。该unit的897条guest指令全部旧覆盖。

## 供实施的边界

- 非空source B若被声明为完整EntryPoint，可覆盖上述18个既有B入口案例；不能登记预算空B、额外exitstub或裸局部label。
- CALL后当前没有queue，需要额外形成后才有source B；仅给现有B加入口并不能覆盖全部CALL后PC。
- 新间接case入口仍可能尚未形成；其新图又进入已存在公共B。合法source B入口与当前域stop-ready必须配合才可能减少这部分重建。此次不模拟剪枝，不推算最终收益。
- 完整入口仍须满足frame/home/flags/fault owner合同；“相同guest范围”本身不是裸label可调用的证明。FEX的CALL continuation入口机制是参考，不是其重复覆盖为零的证据。

产物：cause.json、request-roles.jsonl、analyze-cause.py；全部705次原始事件仍引用既有唯一trace。
