from pathlib import Path
import re

B = Path(__file__).resolve().parent
R = Path('/Volumes/Data2/SwiftVM-submit-20261104')
E = 'evidence/continuation-reuse-20261105/'
text = (B / 'report-draft.md').read_text()
replacements = {
    '# Function已有块入口复用、stop-ready与形成范围收敛 · A3结果草稿':
        '# Function重复构造整改：原因、实现与测量 · 2026-11-05 UTC',
    '**当前仍是候选，不能宣布稳定验收全过。**':
        '**重复构造整改已实现，整体性能验收仍保留候选状态。**',
    'A3覆盖审计及最终关闭信息仍留待root填入。':
        '实际重复覆盖审计、完整入口合同及设备清理结果已闭合。',
    'A3唯一覆盖审计已冻结；最终关闭信息及接收口径仍待root填入。':
        '实际重复覆盖审计、完整入口合同及设备清理结果已闭合。',
    '436个发布入口匹配此前CALL的afterPC': '436个冷root请求匹配此前CALL的afterPC',
    '固定FEX确有CALL-after queue及EntryPoint机制，但它的局部失败边界不同；照搬其发现范围会把当前实现尚不能表示的流拉进大图。':
        '固定FEX确有CALL-after queue及EntryPoint机制，但其指令支持和流表示并不等同于当前SVM；直接移植发现范围会把当前尚不能表示的流拉进大图。',
    '本稿保留candidate/failed-gate状态，最终接收口径由root在保留全部证据的前提下明确。':
        '原固定套件仍保留candidate/failed-gate状态；七轮复核作为单独证据，不重写原验收结果。',
    '## 5. 身份、验证与待填审计': '## 5. 身份、验证与实际重复覆盖',
    '；文件摘要不冒充Git commit，未编造未来提交号。': '；本轮没有提交或push。',
    '本稿只写本证据目录，不改repo/source/设备。': '',
    '相邻[上轮结果文档](/Volumes/Data2/SwiftVM-submit-20261104/docs/compile-owner-and-publication-optimization-2026-11-05.md)用于解释OLD身份及窗口，未在本稿回写。':
        '[上轮结果文档](compile-owner-and-publication-optimization-2026-11-05.md)保留OLD身份及窗口，历史结果没有回写。',
}
for old, new in replacements.items():
    text = text.replace(old, new)
placeholder = '**待root填：最终audit后手机关闭/cleanup回执、最终接收或保留candidate的判定及本地repo/evidence归档位置。** 当前不声称设备已清理或全部目标完成。'
assert placeholder in text
text = text.replace(placeholder,
    '[手机清理回执](attempt3/phone-cleanup-receipt.json)已完成：删除本线私有namespace '
    '59,544,576B，df实际空闲增加60,801,024B，目录剩余0，无活动进程、打开文件或清理错误。'
    '代码位于`/Volumes/Data2/SwiftVM-submit-20261104`，完整源码、压缩runner和诊断副本保留在Data2的'
    '`reconstruction-20261102/continuation-reuse-20261105`；固定构建缓存继续复用。'
    '重复构造修复已经落地，原固定套件的x87 Block compile门仍未由三轮数据通过，'
    '未宣布整个编译性能目标稳定达标。')

def fix_link(match):
    target = match.group(1)
    if target.startswith('attempt3/'):
        target = E + target.removeprefix('attempt3/')
    elif target.startswith(('cause/', 'audit-after/', 'audit-final/', 'audit-accepted-candidate/',
                            'attempt2/', 'x87-paired-confirmation/')) or target == 'final-code-review.md':
        target = E + target
    return '](' + target + ')'

text = re.sub(r'\]\(([^)]+)\)', fix_link, text)
text += ('\n[完整汇总](' + E + 'summary.json)、[同批exclusive成本](' + E + 'compile-breakdown.json)、'
         '[普通原始输出](' + E + 'raw-output.jsonl)、[本轮11源文件补丁](' + E + 'changes-from-buffer-reuse.patch)、'
         '[身份回执](' + E + 'receipt.json)。所有覆盖/回退日志计时均排除于QoR，原有效样本均保留。\n')
assert '待root' not in text and '待填' not in text
output = R / 'docs/function-entry-reuse-2026-11-05.md'
assert not output.exists()
output.write_text(text)
missing = []
for target in re.findall(r'\]\(([^)]+)\)', text):
    if not target.startswith(('http:', 'https:')) and not (output.parent / target.split('#')[0]).exists():
        missing.append(target)
assert not missing, missing
print(output, output.stat().st_size, 'bytes; links verified')
