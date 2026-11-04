# Batch37：四actual单元与真实融合边界

仅B21c3b3/B21c4b1/B21b2d0/Fn21c483各一次，基线分别取已生成34/36/35/36，不重编旧单元。原guest、Known/a64/features63、callable_directory=true及其余Runtime policy打印在JSON。冻结80文件摘要`79c8bf0c2046ab35ece932b5e74310b13d5de339b00ee01e4def117235cdd985`完整核对；工具只改cohort37后重链一次，五库前后SHA一致，锁已释放。无项目build、生产/docs编辑、ADB或新RX绑定。

| 实际单元 | 基线 | N/B | words旧→37 | IR/Home | frame/spill | sites/links |
| --- | --- | ---: | ---: | --- | ---: | ---: |
| B21c3b3 | 34 hot-actual | 11/1 | 29→29 | 逐字同 | 0/0 | 1/0 |
| B21c4b1 | 36 next-two-state-hot | 9/1 | 31→29 | 逐字同 | 0/0 | 0/2 |
| B21b2d0 | 35 hot-actual | 52/1 | 57→55 | 逐字同 | 0/0 | 4/2 |
| Fn21c483 | 36 hot-actual | 84/13 | 148→146 | 逐字同 | 0/0 | 10/0 |

## 32→64 signed-load retarget

B21c3b3旧word2 `LDAPUR W3,[X2]`＋word3 `SXTW X2,W3`变word2 `LDAPURSW X2,[X2]`（99800042）。N6 LoadU32仍Home3/last7，N7 SignExtendU64仍Home2且输入viewU32；原IR/Home不重写。N5地址Home2最后use在load，非writeback的同Rn/Rt无冲突，成功才写X2，同步abort保旧目标。原AF XOR、ADDS、rawPF MOV、CFINV全保，仅提前一字；不是减少flags或改变U32用户语义。

原site guestPC21c3b6/fast2、TEST/PF/carry配方保持，cold仍26，实际为`LDURSW X2,[X2]; DMB ISHLD; B word3`（b8800042）。fast family19800000本次正确走signed load cold，不是store；返回word3现在是AF XOR，跳过的SXT已由load完成。热少一条SXT，whole仍29，因为dispatcher literal gate22→21后新增不可达pad23；DATA仍24。原间接目标Add/完整flags和ready8模板不变。

## 相邻byte发布与MADD

B21c4b1原`SUB W2,W8,#0x3a; UBFX X2,#0,#8; BFXIL X8,X2,#0,#8`现省UBFX。N1是无旗AddU8，唯一数据use为紧邻N2 pinned U8 Store，lastuse恰该Store；后续CMP读新LoadUniform pin8，不是原结果额外use。BFI裁低8并保父高位，原`SXTB W16,W8; MVN W29,W16; ADDS W1,#0xb; SETF8 W1`完整flags保，未向普通多use/whole/raw授dirty许可。热少一字，另少cold DATA pad，31→29；两target21c4d0/21c4c0保，head8→7、ordinary gate9→8，pointer12/24→10/22、fallback14/26→12/24。

B21b2d0首组`MUL W3,W2,W22; EOR W29,W3,W9; ADDS W26,W3,W9`仍原样，Add的完整All95和后续故障PF26/directCarry1保持。仅第二组无旗`MUL W3,W2,W22; ADD W9,W3,W26`在实际word15合成`MADD W9,W2,W22,W26`（1b166849）：乘数2/22、累加26、目标9均由真实Home消费；未覆PF载体26，未把flagfull Add一起吞。热少一字，另少不可达pad，57→55。四fast仍2/5/11/14，guestPC21b2d4/21b2dc/21b2eb/21b2f4及配方保；cold45/48/51/54→43/46/49/52，仍四个signed16 plain＋DMB＋Bfast+1。两target21b311/21b2d0保，ordinary gate23→22、head22→21，pointer26/38→24/36、fallback28/40→26/38。

## Function实际对照与边界

Fn21c483并非unchanged：原word19 byte UBFX被相邻发布规则删除，原54/55的LDAPUR W＋SXTW在本次word53合为LDAPURSW X2；这两条真实删除使148→146，无新MADD。十site原guestPC/TEST/PF/carry配方均同；hot9/11/28/30/37/54/78/79/101/102→9/11/27/29/36/53/76/77/99/100，cold121…145→119…143步长3，signed32 site cold131确为LDURSW X2。其它实际fast/plain/barrier语义保，所有有效cold末B回本次fast+1；内部branch与共享literal真实重定位，零LinkSite仍零。

四IR/Home完全不变，局部frame0不泛化全应用压力。全部词流/recipe/link JSON按实际坐标输出；offline LDR/BR不叫warm，也不把wholewords乘entry hits推动态G。无扩大units、采样/FEX生成或额外测试；普通整批功能与QoR由Root集中验收，观察到此停止。
