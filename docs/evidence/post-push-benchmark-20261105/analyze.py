"""Offline fixed-N5 analysis. No device invocation; require completed cleanup."""
from pathlib import Path
from statistics import median
import datetime
import hashlib
import json
import math
import re
import subprocess
import sys

N = 5
SUBJECT = '390cf73d7d8db5687a8d33cc8be5cdd6aa38638f'
CODE = '7f1f403a9c429c664914867439ef6315ac9f3e3b'
SOURCE = 'a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52'
RUNNER = '13411690d78fa72df6fc84c61004bfe7c36a2060cc3a9a16cb22dcbfca9d77eb'
PLAN_SHA = '39fcc0cc4bb042585a13820a7d52489e0119824902909ec1a1638d0195a3ca72'
CASES = ['coremark', 'int', 'fp', 'mem', 'branch', 'call', 'func_tests', 'x87_bench']
ARMS = ['block', 'function', 'fex']
LIMITS = {'seconds': 1.1, 'cycles': 1.1, 'compile': 2.0}
PMU = {'cycles': 'cpu-cycles:u', 'instructions': 'instructions:u', 'branch_misses': 'branch-misses:u'}
FEX_GROUPS = {
    'formation_and_ir': ['raw_decode', 'builder', 'generate_ir'],
    'ir_optimization': ['pass_manager', 'x87', 'dfe', 'validation', 'other_pass'],
    'allocation_and_encoding': ['ra', 'backend', 'emit'],
    'transfer_and_publication': ['tail_copy_cache', 'publish'],
    'logical_work_buffer_release': ['logical_release'],
    'other': ['compile_block', 'compile_code'],
}
WORK = [
    'compile_attempts', 'function_attempts', 'block_attempts', 'fallbacks',
    'code_units_published', 'emitted_guest_instructions', 'emitted_guest_bytes',
    'ir_nodes', 'operand_words', 'native_bytes', 'peak_cache_entries',
    'peak_cache_occupied', 'peak_cache_slots', 'cache_slot_bytes',
    'cache_table_allocations', 'native_code_bytes', 'peak_code_virtual_bytes',
    'peak_slabs', 'peak_dual_slabs', 'spill_slots', 'max_frame_bytes', 'fault_sites',
    'static_links', 'patched_links', 'pending_links', 'pending_targets',
    'link_relocation_rows', 'link_relocation_capacity', 'compile_decode_calls',
    'dispatches', 'cache_hits', 'directory_probe_steps', 'cache_invalidations',
    'syscalls', 'unsupported_syscalls', 'unaligned_recoveries', 'unaligned_patches',
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def number(value, label, positive=False):
    require(type(value) in (int, float) and math.isfinite(value), label + ': not finite numeric')
    require(value > 0 if positive else value >= 0, label + ': invalid sign')
    return value


def grid(rows, kind):
    expected = {(c, a, r) for c in CASES for a in ARMS for r in range(N)}
    require(len(rows) == 120, kind + ': expected exactly120 records')
    require(all(type(x.get('round')) is int for x in rows), kind + ': invalid round type')
    require({(x['case'], x['arm'], x['round']) for x in rows} == expected, kind + ': incomplete/duplicate Cartesian grid')
    require(all(x.get('valid') is True for x in rows), kind + ': invalid observations retained')
    for x in rows:
        require(x['prefix'] == f"{kind}-{x['case']}-{x['arm']}-{x['round']}", kind + ': prefix mismatch')


def exclusive(row):
    rec = row['record']
    total = number(row['compile_excluding_byte_ns'], 'compile total', True)
    extra = {}
    if row['arm'] == 'fex':
        require(rec.get('qualified') is True and rec.get('closed') is True and rec.get('clock_supported') is True,
                'FEX probe not qualified/closed')
        for key in ['stack_errors', 'arithmetic_errors', 'unclassified_pass_runs', 'reentries']:
            require(rec[key] == 0, 'FEX probe error: ' + key)
        stages = {x['name']: x for x in rec['stages']}
        require(len(stages) == len(rec['stages']), 'duplicate FEX stage')
        for s in stages.values():
            for key in ['ticks', 'children_ticks', 'remaining_ticks']:
                number(s[key], 'FEX ' + key)
            require(s['ticks'] - s['children_ticks'] == s['remaining_ticks'], 'FEX exclusive stage mismatch')
        require(sum(s['remaining_ticks'] for s in stages.values()) == rec['root_ticks'] == rec['exclusive_ticks'],
                'FEX whole tick ledger does not close')
        byte = stages['byte_decode']
        require(byte['children_ticks'] == 0, 'byte decode scope includes nested work')
        freq = number(rec['cntfrq_hz'], 'FEX cntfrq', True)
        ticks = {k: sum(stages[n]['remaining_ticks'] for n in names) for k, names in FEX_GROUPS.items()}
        require(sum(ticks.values()) == rec['root_ticks'] - byte['ticks'], 'FEX grouped ledger does not close')
        parts = {k: v * 1000 / freq for k, v in ticks.items()}
        extra = {'exclusive_ticks': ticks, 'cntfrq_hz': freq, 'root_calls': rec['root_calls'],
                 'byte_decode_calls': byte['calls'], 'exact_tick_closure': True}
    else:
        require(rec['runtime_completed'] is True and rec['exit_code'] == (101 if row['case'] == 'func_tests' else 0),
                'SVM compile launch did not complete')
        require(rec['compile_without_byte_decode_ns'] == total == rec['compile_ns'] - rec['compile_decode_ns'],
                'SVM byte exclusion mismatch')
        parts = {'formation_and_ir': (rec['frontend_ns'] - rec['compile_decode_ns']) / 1e6,
                 'ir_optimization': rec['optimize_ns'] / 1e6,
                 'allocation_and_encoding': rec['backend_ns'] / 1e6,
                 'transfer_and_publication': rec['publication_ns'] / 1e6,
                 'native_owner_release': rec['code_release_ns'] / 1e6}
        parts['other'] = total / 1e6 - sum(parts.values())
        extra = {'fault_service_ms_in_other': rec['fault_service_ns'] / 1e6}
    require(all(math.isfinite(v) and v >= -1e-12 for v in parts.values()), 'negative/nonfinite exclusive component')
    error = sum(parts.values()) - total / 1e6
    require(abs(error) < 1e-8, 'exclusive sum differs from total')
    return {'round': row['round'], 'prefix': row['prefix'], 'total_ms': total / 1e6,
            'exclusive_ms': parts, 'closure_error_ms': error, **extra}


def main(p):
    provenance = {}
    def read(name):
        path = p / name
        require(path.is_file(), 'MISSING prerequisite: ' + name)
        data = path.read_bytes()
        provenance[name] = hashlib.sha256(data).hexdigest()
        return json.loads(data)

    # This receipt is deliberately required before reading or aggregating samples.
    cleanup = read('phone-cleanup-receipt.json')
    require(cleanup['namespace'] == '/data/local/tmp/swiftvm-post-push-20261105', 'wrong cleanup namespace')
    require(cleanup['remaining_bytes'] == 0 and all(cleanup[k] == [] for k in ['active_processes', 'open_files', 'errors']),
            'namespace cleanup not closed')
    require(cleanup['allocated_bytes_removed'] > 0, 'cleanup did not account for owned bytes')
    plan, identity, manifest = read('plan.json'), read('identity.json'), read('source-manifest.json')
    require(provenance['plan.json'] == PLAN_SHA, 'registered plan changed')
    require(plan['subject_commit'] == plan['remote_commit_verified'] == SUBJECT and plan['code_commit'] == CODE,
            'plan commit mismatch')
    require(plan['source_digest'] == SOURCE and plan['source_files'] == 118 and plan['repetitions_per_case_arm'] == N,
            'plan source/N mismatch')
    require(plan['workloads'] == CASES and plan['arms'] == ARMS and plan['runtime_samples'] == plan['compile_samples'] == 120,
            'plan Cartesian mismatch')
    require(plan['known_limits'] == {'time_ratio': 1.1, 'user_cycle_ratio': 1.1, 'compile_ratio': 2.0}, 'plan limits changed')
    require(plan['cpu'] == 4 and plan['device'] == 'bf1359ff' and plan['min_khz'] == 614400 and plan['max_khz'] == 1171200,
            'plan environment mismatch')
    require(sha(p / 'run.py') == plan['harness_sha256'] and provenance['source-manifest.json'] == plan['source_manifest_sha256'],
            'registered harness/manifest changed')
    require(identity['base_commit'] == identity['subject_commit'] == identity['remote_commit_verified'] == SUBJECT,
            'identity is not the measured post-push subject')
    require(identity['code_commit'] == CODE and identity['source_digest'] == SOURCE and identity['repetitions'] == N,
            'identity source/code/N mismatch')
    require(identity['measurement_plan_sha256'] == PLAN_SHA and identity['working_tree_modified'] is False
            and identity['no_optional_stopping'] is True and identity['prior_samples_not_pooled'] is True,
            'identity registration contract incomplete')
    require(identity['device'] == 'bf1359ff' and identity['cpu'] == 4, 'identity target mismatch')
    repo = Path(manifest['source_worktree'])
    require(repo == Path('/Volumes/Data2/SwiftVM-submit-20261104'), 'unexpected worktree')
    git = lambda *args: subprocess.check_output(['git', '--no-optional-locks', *args], cwd=repo, text=True).strip()
    require(git('rev-parse', 'HEAD') == SUBJECT and git('rev-parse', CODE + '^{commit}') == CODE, 'actual git commit mismatch')
    require(git('status', '--porcelain', '--untracked-files=all') == '', 'current git worktree is not clean')
    require(manifest['base_commit'] == manifest['subject_commit'] == SUBJECT and manifest['code_commit'] == CODE
            and manifest['working_tree_modified'] is False, 'manifest commit mismatch')
    files = manifest['files']
    require(len(files) == 118 and len({x['path'] for x in files}) == 118, 'source closure is not exactly118 unique files')
    current_changed, frozen_changed = [], []
    for x in files:
        rel = Path(x['path'])
        require(not rel.is_absolute() and '..' not in rel.parts, 'invalid source path')
        for root, changed in [(repo, current_changed), (p / 'source', frozen_changed)]:
            path = root / rel
            if not path.is_file() or path.stat().st_size != x['bytes'] or sha(path) != x['sha256']:
                changed.append(x['path'])
    digest = hashlib.sha256(''.join(x['path'] + '\0' + x['sha256'] + '\n'
                                    for x in sorted(files, key=lambda x: x['path'])).encode()).hexdigest()
    require(digest == manifest['digest'] == SOURCE and not current_changed and not frozen_changed, 'source closure drift')
    # Read-only check that the subject's118 source files equal the code commit.
    require(subprocess.run(['git', '--no-optional-locks', 'diff', '--quiet', CODE, SUBJECT, '--',
                            *[x['path'] for x in files]], cwd=repo).returncode == 0, 'subject changed code-commit sources')
    source_closure = {'digest': SOURCE, 'files': 118, 'changed': [], 'current_changed': current_changed,
                      'frozen_changed': frozen_changed, 'digest_recomputed': digest,
                      'manifest_sha256': provenance['source-manifest.json'], 'current_head': SUBJECT,
                      'subject_commit': SUBJECT, 'code_commit': CODE}
    inputs = read('inputs.json')
    require(len(inputs) == 14 and inputs['candidate']['sha256'] == RUNNER, 'wrong deployment/candidate identity')
    def remote(name):
        path = p / name
        require(path.is_file(), 'MISSING prerequisite: ' + name)
        provenance[name] = sha(path)
        rows = [line.split(maxsplit=1) for line in path.read_text().splitlines() if line.strip()]
        require(len(rows) == 14 and all(len(x) == 2 for x in rows), name + ': expected14 identities')
        result = {name.strip(): digest for digest, name in rows}
        require(len(result) == 14, name + ': duplicate identity')
        return result
    expected_remote = {cleanup['namespace'] + '/' + name: row['sha256'] for name, row in inputs.items()}
    require(remote('remote-identities-before.stdout') == expected_remote == remote('remote-identities-after.stdout'),
            'deployed14-input identities differ')
    for name, row in inputs.items():
        path = Path(row['path'])
        require(path.is_file() and path.stat().st_size == row['bytes'] and sha(path) == row['sha256'],
                'local input changed: ' + name)
    telemetry = {}
    for name in ['prepare-policy', 'smoke-policy-before', 'smoke-policy-after',
                 'compile-policy-before', 'compile-policy-after', 'runtime-policy-before', 'runtime-policy-after']:
        path = p / (name + '.stdout')
        require(path.is_file(), 'MISSING prerequisite: ' + path.name)
        provenance[path.name] = sha(path)
        vals = [int(x) for x in path.read_text().split()]
        require(len(vals) == 4 and vals[:2] == [614400, 1171200] and vals[2] > 0, 'CPU policy telemetry changed: ' + name)
        telemetry[name] = dict(zip(['min_khz', 'max_khz', 'observed_khz', 'battery_temperature_raw'], vals))
    smoke, modes = read('smoke-summary.json'), read('mode-probe-summary.json')
    require(len(smoke) == 18 and all(x['valid'] is True for x in smoke), '18 functional smoke observations incomplete')
    require({(x['case'], x.get('arm')) for x in smoke} ==
            {(c, a) for c in CASES for a in ['block', 'function']} | {('backend-test', None), ('pipeline-test', None)},
            'functional smoke Cartesian mismatch')
    require(len(modes) == 2 and {x['arm'] for x in modes} == {'block', 'function'} and
            all(x['valid'] is True and x['exit'] == 0 and x['record']['cache_invalidations'] >= 1
                and x['record']['smc_write_faults'] >= 1 for x in modes), 'FP mode/SMC probes incomplete')
    case_defs = read('cases.json')
    require([x['name'] for x in case_defs] == CASES, 'workload list changed')
    runtime, comp = read('runtime-records.json'), read('compile-records.json')
    grid(runtime, 'runtime'); grid(comp, 'compile')
    affinity = {'wrapper_cpu4_verified': 0, 'actual_live_verified': 0, 'wrapper_only': [],
                'proof_boundary': 'All launches use recorded taskset10 and wrapper CPU4 status; children inherit affinity. '
                                  'A live target status is checked only when the fixed150ms probe captured it.'}
    for x in runtime:
        expected_exit = 101 if x['case'] == 'func_tests' else 0
        require(x['exit'] == expected_exit and x['profiler_exit'] in {0, expected_exit}, 'runtime exit mismatch')
        require(x['cpu_allowed_list'] == '4' and ' -- taskset 10 timeout ' in x['command'], 'runtime affinity command mismatch')
        meta_path = p / (x['prefix'] + '-meta.stdout')
        require(meta_path.is_file(), 'MISSING wrapper metadata: ' + x['prefix'])
        provenance[meta_path.name] = sha(meta_path)
        meta = meta_path.read_text()
        header = meta.splitlines()
        mask = re.search(r'^Cpus_allowed_list:\s*(.+)$', meta, re.M)
        require(len(header) >= 2 and int(header[0]) == x['exit'] and int(header[1]) > 0
                and mask and mask[1].strip() == '4', 'wrapper PID/exit/CPU4 mismatch')
        affinity['wrapper_cpu4_verified'] += 1
        live_path = p / (x['prefix'] + '-live.stdout')
        require(live_path.is_file(), 'MISSING fixed live-probe output: ' + x['prefix'])
        provenance[live_path.name] = sha(live_path)
        live_mask = re.search(r'^Cpus_allowed_list:\s*(.+)$', live_path.read_text(), re.M)
        require(bool(live_mask) == x['live_pid_verified'], 'record/live-probe availability mismatch')
        if live_mask:
            require(live_mask[1].strip() == '4', 'live target is not CPU4')
            affinity['actual_live_verified'] += 1
        else:
            affinity['wrapper_only'].append({k: x[k] for k in ['case', 'arm', 'round', 'prefix', 'seconds']})
        if x['case'] == 'coremark':
            require(x['seconds'] >= 10, 'CoreMark primary duration below10s')
        for key in ['seconds', 'whole_child_seconds']:
            number(x[key], 'runtime ' + key, True)
        for key in PMU.values():
            number(x['pmu'][key], key, True)
    all_parts = {x['prefix']: exclusive(x) for x in comp}  # Check all120 ledgers, not only selected samples.
    baseline_path = repo / 'docs/evidence/continuation-reuse-20261105/summary.json'
    baseline = json.loads(baseline_path.read_text())
    require(baseline['identity']['source_digest'] == SOURCE and baseline['inputs']['candidate']['sha256'] == RUNNER,
            'prior checkpoint does not have the same source/ordinary runner')
    for key in ['FEXLoader', 'FEXLoader-byte', 'FEXServer', 'simpleperf', 'coremark',
                'bench-suite', 'func-tests', 'x87-bench', 'fex-config/Config.json']:
        require(inputs[key]['sha256'] == baseline['inputs'][key]['sha256'], 'fixed reference/input changed: ' + key)
    result = {'schema': 'svm_post_push_fixed_n5_v1', 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'identity': identity, 'inputs': inputs, 'plan': plan, 'runtime': {}, 'compile': {}, 'ratios': {},
              'source_closure': source_closure, 'phone_cleanup': cleanup, 'deployed_inputs_unchanged': True,
              'remote_inputs_verified': 14, 'telemetry': telemetry,
              'baseline': {'path': str(baseline_path), 'sha256': sha(baseline_path), 'n': 3,
                           'comparison_kind': 'same source and ordinary runner; historical observations, not pooled',
                           'original_acceptance': baseline['acceptance']}}
    breakdown = {'method': 'Each case/arm selects its total-excluding-byte median observation (sorted index2 of5). '
                 'Exclusive groups use only that observation; all120 ledgers checked. FEX uses remaining_ticks. '
                 'SVM other is a nonnegative residual. Native-owner and FEX logical-buffer release scopes differ.', 'cases': {}}
    comparison = {'baseline': result['baseline'], 'method': 'new5 versus historical3, no pooling or hunk attribution', 'cases': {}}
    for c in CASES:
        result['runtime'][c], result['compile'][c], result['ratios'][c] = {}, {}, {}
        breakdown['cases'][c], comparison['cases'][c] = {}, {}
        for a in ARMS:
            rs = sorted([x for x in runtime if x['case'] == c and x['arm'] == a], key=lambda x: x['round'])
            cs = sorted([x for x in comp if x['case'] == c and x['arm'] == a], key=lambda x: x['round'])
            rr = {'n': N, 'seconds': median(x['seconds'] for x in rs),
                  'min_seconds': min(x['seconds'] for x in rs), 'max_seconds': max(x['seconds'] for x in rs),
                  'whole_child_seconds': median(x['whole_child_seconds'] for x in rs),
                  **{k: median(x['pmu'][v] for x in rs) for k, v in PMU.items()},
                  'ipc': median(x['pmu']['instructions:u'] / x['pmu']['cpu-cycles:u'] for x in rs),
                  'samples': [{k: x[k] for k in ['round', 'prefix', 'seconds', 'whole_child_seconds', 'pmu', 'exit',
                                                 'valid', 'cpu_allowed_list', 'live_pid_verified']} for x in rs]}
            selected = sorted(cs, key=lambda x: (x['compile_excluding_byte_ns'], x['round']))[N // 2]
            part = all_parts[selected['prefix']]
            cc = {'n': N, 'ms': part['total_ms'], 'min_ms': min(x['compile_excluding_byte_ns'] for x in cs) / 1e6,
                  'max_ms': max(x['compile_excluding_byte_ns'] for x in cs) / 1e6,
                  'selected_round': selected['round'], 'selected_prefix': selected['prefix'],
                  'exclusive_ms': part['exclusive_ms'],
                  'samples': [{'round': x['round'], 'prefix': x['prefix'],
                               'total_ms': x['compile_excluding_byte_ns'] / 1e6} for x in cs]}
            if a != 'fex':
                rec = selected['record']
                cc['work'] = {k: rec[k] for k in WORK}
                cc['work']['ir_work_counts_include_rejected_prefixes'] = rec['ir_work_counts_include_rejected_prefixes']
                cc['work']['successful_function_units'] = rec['function_attempts'] - rec['fallbacks']
                cc['work']['peak_slot_array_element_bytes'] = rec['peak_cache_slots'] * rec['cache_slot_bytes']
                cc['work_distribution'] = {k: {'values': [x['record'][k] for x in cs],
                                             'min': min(x['record'][k] for x in cs),
                                             'max': max(x['record'][k] for x in cs)} for k in WORK}
                cc['work_scope'] = 'Actual selected record; IR N/arena words exclude rejected IR prefixes. '
                cc['work_scope'] += 'Compile totals still charge all failed attempts/retries. Short CoreMark probe uses20 iterations.'
            else:
                cc['root_calls'] = part['root_calls']
                cc['root_calls_distribution'] = [x['record']['root_calls'] for x in cs]
            result['runtime'][c][a], result['compile'][c][a] = rr, cc
            breakdown['cases'][c][a] = part
            before_r, before_c = baseline['runtime'][c][a], baseline['compile'][c][a]
            comparison['cases'][c][a] = {
                'before_n': before_r['n'], 'after_n': N,
                'before_runtime': {k: before_r[k] for k in ['seconds', 'cycles', 'instructions', 'branch_misses']},
                'after_runtime': {k: rr[k] for k in ['seconds', 'cycles', 'instructions', 'branch_misses']},
                'runtime_change_fraction': {k: rr[k] / before_r[k] - 1 for k in ['seconds', 'cycles', 'instructions', 'branch_misses']},
                'before_compile_ms': before_c['ms'], 'after_compile_ms': cc['ms'],
                'compile_change_fraction': cc['ms'] / before_c['ms'] - 1,
                'before_work': before_c.get('work'), 'after_work': cc.get('work')}
        for a in ['block', 'function']:
            r, f = result['runtime'][c][a], result['runtime'][c]['fex']
            ratios = {k: r[k] / f[k] for k in ['seconds', 'cycles', 'instructions', 'branch_misses']}
            ratios['compile'] = result['compile'][c][a]['ms'] / result['compile'][c]['fex']['ms']
            result['ratios'][c][a] = ratios
    failures = [{'case': c, 'arm': a, 'metric': k, 'ratio': result['ratios'][c][a][k], 'limit': limit}
                for c in CASES for a in ['block', 'function'] for k, limit in LIMITS.items()
                if result['ratios'][c][a][k] > limit]
    result['acceptance'] = {'limits': LIMITS, 'passed': not failures, 'failures': failures, 'n': N,
                            'gated_checks': 48, 'passed_checks': 48 - len(failures),
                            'runtime_valid': 120, 'compile_valid': 120,
                            'time_kind': 'CoreMark loop; other cases whole-child including startup/JIT; all8 cases gated'}
    result['worst_ratios'] = {k: max(result['ratios'][c][a][k] for c in CASES for a in ['block', 'function']) for k in LIMITS}
    result['status'] = 'fixed_n5_gates_pass' if not failures else 'fixed_n5_gates_failed'
    result['compile_breakdown'], result['comparison'] = 'compile-breakdown.json', 'comparison.json'
    result['validation'] = {'cartesian_runtime': 120, 'cartesian_compile': 120, 'exclusive_ledgers_closed': 120,
                            'functional_smoke_valid': 18, 'mode_probes_valid': 2, 'cleanup_closed': True,
                            'affinity': affinity,
                            'registered_plan_sha256': PLAN_SHA, 'input_sha256': provenance,
                            'analysis_script_sha256': sha(Path(__file__)),
                            'no_samples_pooled_or_replaced': True}
    result['limitations'] = [
        'All8 fixed cases retain all48 original gates. Instructions/branch misses are diagnostics, not new gates.',
        'Runtime/PMU metrics are separate five-observation medians; ratios use same-round-cohort FEX medians.',
        'Five samples give no blanket significance/stability/all-application guarantee. CPU is not frequency locked.',
        'CoreMark runtime100000 iterations; compile/work probe20 iterations. No cross-workload average.',
        'func_tests retains native stdout/exit101 oracle and performance gates; x87 is exit-only, not numerical validation.',
        'Native-owner versus FEX logical-working-buffer release is not an identical destruction endpoint.',
        'Prior3 and supplementary7-per-arm results are historical only; identical code changes are not claimed.']
    # Recheck the small input set before publishing derived files. Never modify raw evidence.
    for name, digest in provenance.items():
        require(sha(p / name) == digest, 'input changed during analysis: ' + name)
    for name, value in [('summary.json', result), ('compile-breakdown.json', breakdown), ('comparison.json', comparison)]:
        target = p / name
        temporary = target.with_name(target.name + '.tmp')
        temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')
        temporary.replace(target)
    print(result['status'], f"{48 - len(failures)}/48", 'all240 observations valid; cleanup closed')
    for x in failures:
        print('FAIL', x['case'], x['arm'], x['metric'], x['ratio'], 'limit', x['limit'])
    for c in CASES:
        print(c, json.dumps(result['ratios'][c]))


if __name__ == '__main__':
    try:
        require(len(sys.argv) <= 2, 'usage: analyze.py [evidence-directory]')
        main(Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else Path(__file__).resolve().parent)
    except (ValueError, KeyError, OSError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        raise SystemExit('ANALYSIS INCOMPLETE: ' + str(error))
