#!/usr/bin/env python3
"""Derive two cold-log objects and a runner; never build or modify the cache."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = Path('/Volumes/Data2/SwiftVM-submit-20261104')
CACHE = Path('/Volumes/Data2/swiftvm-middle-build-cache/reconstruction/submit-android')
NINJA = '/opt/homebrew/bin/ninja'
SOURCES = {
    'runtime': 'source/runtime/execution/runtime.cpp',
    'build_ir': 'source/runtime/frontend/x86/build_ir.cpp',
}
TARGETS = {
    'runtime': 'CMakeFiles/svm_execution.dir/source/runtime/execution/runtime.cpp.o',
    'build_ir': 'CMakeFiles/svm_x86_ir.dir/source/runtime/frontend/x86/build_ir.cpp.o',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError('source anchor changed: ' + old[:100])
    return source.replace(old, new, 1)


def log_line(event, fields, arguments, indent='        '):
    fmt = '{"function_audit":"' + event + '",' + fields + '}\n'
    return indent + 'std::fprintf(stderr, ' + json.dumps(fmt) + ',\n' + indent + '        ' + arguments + ');\n'


def instrument_runtime(source):
    seq = 'static_cast<unsigned long long>(stats.compile_attempts)'
    pc = 'static_cast<unsigned long long>(state.pc)'
    base = 'static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(code.entry))'
    anchor = '        ++stats.compile_attempts;\n'
    source = once(source, anchor, anchor + log_line('begin', '"seq":%llu,"pc":%llu,"scope":"%s","fp_mode":%u',
        seq + ', ' + pc + ', scope == ir::Scope::Function ? "function" : "block", static_cast<unsigned>(fp_mode)'))
    anchor = '        stats.emitted_instructions += built.instruction_count; stats.emitted_guest_bytes += built.decoded_bytes;\n'
    source = once(source, anchor, anchor + log_line('front',
        '"seq":%llu,"ok":%s,"error":%u,"ir_error":%u,"error_pc":%llu,"guest_instructions":%zu,"guest_bytes":%zu,"nodes_available":%s,"nodes":%zu,"words":%zu,"mutable_entry":%s',
        seq + ', built ? "true" : "false", static_cast<unsigned>(built.error), static_cast<unsigned>(built.ir_error), static_cast<unsigned long long>(built.error_pc), built.instruction_count, built.decoded_bytes, built.program ? "true" : "false", built.program.Operations().size(), built.program.OperandArena().size(), mutable_entry ? "true" : "false"'))
    anchor = '            built.program = std::move(optimized.program);\n'
    source = once(source, anchor, log_line('optimize', '"seq":%llu,"ok":%s,"error":%u,"nodes":%zu,"words":%zu',
        seq + ', optimized ? "true" : "false", static_cast<unsigned>(optimized.error), optimized.program.Operations().size(), optimized.program.OperandArena().size()', '            ') + anchor)
    anchor = '        { Timer backend{stats.backend_ns}; arm64::Compile(built.program, policy, compiled); }\n'
    source = once(source, anchor, anchor + log_line('backend',
        '"seq":%llu,"ok":%s,"error":%u,"node":%u,"block":%u,"nodes":%zu,"words":%zu,"native_bytes":%zu,"extra_entry_count":%zu',
        seq + ', compiled ? "true" : "false", static_cast<unsigned>(compiled.error), compiled.node.value, compiled.block.value, built.program.Operations().size(), built.program.OperandArena().size(), compiled.words.size() * 4, compiled.entries.size()'))
    anchor = '        // Keep only the actual working buffers; no native metadata borrows\n'
    source = once(source, anchor, '        const auto audit_nodes = built.program.Operations().size();\n        const auto audit_words = built.program.OperandArena().size();\n' + anchor)
    anchor = '        return code;\n    }\n    bool ReadString('
    source = once(source, anchor, log_line('publish',
        '"seq":%llu,"root_pc":%llu,"owner_base":%llu,"nodes":%zu,"words":%zu,"native_bytes":%zu,"extra_entry_count":%zu,"ir_reuse_reserved_bytes":%zu,"word_capacity_bytes":%zu,"link_capacity_bytes":%zu,"entry_capacity_bytes":%zu',
        seq + ', ' + pc + ', ' + base + ', audit_nodes, audit_words, code.bytes, compiled.entries.size(), ir_reuse.ReservedBytes(), compiled.words.capacity() * sizeof(std::uint32_t), compiled.links.capacity() * sizeof(arm64::LinkSite), compiled.entries.capacity() * sizeof(arm64::EntryPoint)') + anchor)
    anchor = '                Cache().InsertReady(state.pc, code, compile_output.entries);\n'
    body = '                for (const auto& audit_entry : compile_output.entries) {\n                    NativeCode actual_owner;\n                    const auto actual = Cache().Find(audit_entry.pc, &actual_owner);\n                    const auto offered = reinterpret_cast<std::uintptr_t>(code.entry) + std::uintptr_t{audit_entry.word} * 4;\n'
    body += log_line('entry',
        '"seq":%llu,"root_pc":%llu,"pc":%llu,"word":%u,"owner_base":%llu,"actual_entry":%llu,"actual_owner_base":%llu,"accepted_here":%s',
        seq + ', ' + pc + ', static_cast<unsigned long long>(audit_entry.pc), audit_entry.word, ' + base + ', static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(actual)), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(actual_owner.entry)), reinterpret_cast<std::uintptr_t>(actual) == offered ? "true" : "false"', '                    ')
    body += '                }\n'
    source = once(source, anchor, anchor + body)
    anchor = '            entry = code.entry; // The root remains at unit offset zero.\n'
    source = once(source, anchor, log_line('ready',
        '"seq":%llu,"pc":%llu,"fp_mode":%u,"native_bytes":%zu,"root_word":0,"owner_base":%llu,"entries_capacity_bytes":%zu',
        seq + ', ' + pc + ', static_cast<unsigned>(fp_mode), code.bytes, ' + base + ', compile_output.entries.capacity() * sizeof(arm64::EntryPoint)', '            ') + anchor)
    anchor = '                            return bool(static_cast<CodeCache*>(cache)->Find(pc));\n'
    body = '                            NativeCode audit_owner;\n                            const auto audit_entry = static_cast<CodeCache*>(cache)->Find(pc, &audit_owner);\n                            if (audit_entry) {\n'
    body += log_line('stop_ready', '"pc":%llu,"entry":%llu,"owner_base":%llu',
        'static_cast<unsigned long long>(pc), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(audit_entry)), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(audit_owner.entry))', '                                ')
    body += '                            }\n                            return bool(audit_entry);\n'
    return once(source, anchor, body)


def instrument_frontend(source):
    source = once(source, '#include <chrono>\n', '#include <chrono>\n#include <cstdio>\n')
    anchor = '    u64 pc = blocks.front().pc;\n'
    source = once(source, anchor, anchor + '    unsigned audit_opcode{}; u64 audit_opcode_pc = UINT64_MAX;\n')
    anchor = '                const auto& instruction = instructions[slice.first_instruction + i];\n'
    source = once(source, anchor, anchor + '                audit_opcode = instruction.opcode; audit_opcode_pc = instruction.addr;\n')
    anchor = '    } catch (const Rejected& rejected) {\n'
    source = once(source, anchor, anchor + log_line('rejected_prefix',
        '"error":%u,"ir_error":%u,"pc":%llu,"guest_instructions":%zu,"guest_bytes":%zu,"nodes":%zu,"words":null,"opcode_available":%s,"opcode":%u',
        'static_cast<unsigned>(rejected.error), static_cast<unsigned>(rejected.detail), static_cast<unsigned long long>(pc), instruction_count, decoded, builder.Operations().size(), audit_opcode_pc == pc ? "true" : "false", audit_opcode'))
    start = source.index('    const auto decoded = DecodeFunction(readable, entry_pc, limits, timing,')
    finish = source.index('    if (!decoded)', start)
    anchor = source[start:finish]
    body = log_line('formation',
        '"entry":%llu,"error":%u,"error_pc":%llu,"instructions":%zu,"guest_bytes":%zu,"decode_calls":%zu,"blocks":%zu,"splits":%zu,"budget_exits":%zu',
        'static_cast<unsigned long long>(entry_pc), static_cast<unsigned>(decoded.error), static_cast<unsigned long long>(decoded.error_pc), decoded.instructions.size(), decoded.decoded_bytes, decoded.decode_calls, decoded.blocks.size(), decoded.splits, decoded.budget_exits', '    ')
    body += '    for (const auto& slice : decoded.blocks) {\n        u64 end = slice.pc;\n        if (slice.instruction_count) {\n            const auto& last = decoded.instructions[std::size_t{slice.first_instruction} + slice.instruction_count - 1];\n            end = last.addr + last.size;\n        }\n'
    body += log_line('block', '"pc":%llu,"end":%llu,"count":%u', 'static_cast<unsigned long long>(slice.pc), static_cast<unsigned long long>(end), slice.instruction_count')
    body += '    }\n'
    return once(source, anchor, anchor + body)


def prepare():
    manifest = {'kind': 'cold_compilation_diagnostic_not_a_timing_baseline', 'root': str(ROOT), 'cache': str(CACHE), 'sources': []}
    production_manifest = HERE.parent / 'attempt3/source-manifest.json'
    manifest['production_source_digest'] = json.loads(production_manifest.read_text())['digest']
    manifest['production_source_manifest_sha256'] = digest(production_manifest)
    patch = []
    for label, relative in SOURCES.items():
        original = ROOT / relative
        text = original.read_text()
        changed = instrument_runtime(text) if label == 'runtime' else instrument_frontend(text)
        before, after = HERE / 'before' / relative, HERE / 'source' / relative
        before.parent.mkdir(parents=True, exist_ok=True)
        after.parent.mkdir(parents=True, exist_ok=True)
        before.write_text(text)
        after.write_text(changed)
        manifest['sources'].append({'path': relative, 'source_sha256': digest(original), 'diagnostic_sha256': digest(after)})
        patch.extend(difflib.unified_diff(text.splitlines(keepends=True), changed.splitlines(keepends=True), fromfile='a/' + relative, tofile='b/' + relative))
    (HERE / 'diagnostic.patch').write_text(''.join(patch))
    manifest['patch_sha256'] = digest(HERE / 'diagnostic.patch')
    (HERE / 'prepared.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def command(target, contains):
    lines = subprocess.run([NINJA, '-C', str(CACHE), '-t', 'commands', target], check=True, capture_output=True, text=True).stdout.splitlines()
    matches = [part for line in lines for part in line.split(' && ') if contains in part and 'clang++' in part]
    if len(matches) != 1:
        raise RuntimeError('expected one cached command for ' + target)
    return shlex.split(matches[0])


def replace_argument(argv, flag, value):
    index = argv.index(flag)
    argv[index + 1] = str(value)


def build(manifest):
    objects = HERE / 'objects'
    objects.mkdir(exist_ok=True)
    (HERE / 'tmp').mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment['TMPDIR'] = str(HERE / 'tmp')
    commands, overrides = [], []
    for label, relative in SOURCES.items():
        argv = command(TARGETS[label], ' -c ' + str(ROOT / relative))
        obj = objects / (label + '.o')
        replace_argument(argv, '-o', obj)
        replace_argument(argv, '-c', HERE / 'source' / relative)
        replace_argument(argv, '-MF', objects / (label + '.o.d'))
        replace_argument(argv, '-MT', obj)
        argv += ['-iquote', str((ROOT / relative).parent)]
        commands.append(argv)
        overrides.append(str(obj))
    link = command('svm_rebuild_linux', ' -o svm_rebuild_linux ')
    binary = HERE / 'svm_rebuild_linux_function_audit'
    replace_argument(link, '-o', binary)
    link = [('--dependency-file=' + str(HERE / 'link.d')) if value.startswith('--dependency-file=') else value for value in link]
    first_archive = next(i for i, value in enumerate(link) if value.endswith('libsvm_execution.a'))
    link[first_archive:first_archive] = overrides
    commands.append(link)
    dependencies = [CACHE / value for value in link if value.endswith(('.a', '.o')) and not value.startswith(str(HERE))]
    before = {str(path): digest(path) for path in dependencies}
    (HERE / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    with (HERE / 'build.log').open('w') as log:
        for argv in commands:
            log.write(shlex.join(argv) + '\n')
            log.flush()
            subprocess.run(argv, cwd=CACHE, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
    after = {str(path): digest(path) for path in dependencies}
    if before != after:
        raise RuntimeError('ordinary cache inputs changed during diagnostic build')
    if any(digest(ROOT / row['path']) != row['source_sha256'] for row in manifest['sources']):
        raise RuntimeError('ordinary source changed during diagnostic build')
    manifest.update({'binary': str(binary), 'binary_sha256': digest(binary), 'binary_bytes': binary.stat().st_size,
                     'objects': {Path(path).name: digest(Path(path)) for path in overrides}, 'ordinary_inputs_unchanged': True,
                     'ordinary_inputs_sha256': before, 'build_exit_code': 0})
    (HERE / 'receipt.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'binary': str(binary), 'sha256': manifest['binary_sha256'], 'bytes': manifest['binary_bytes']}))


if __name__ == '__main__':
    prepared = prepare()
    if sys.argv[1:] == ['--prepare-only']:
        print(HERE / 'prepared.json')
    elif not sys.argv[1:]:
        build(prepared)
    else:
        raise SystemExit('usage: build-diagnostic.py [--prepare-only]')
