#!/usr/bin/env python3
"""Offline cold-coverage/registration audit; never executes guest or compiler code."""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct

import capstone

HERE = Path(__file__).resolve().parent
EVIDENCE = HERE.parents[1]
BASE = EVIDENCE / "compile-owner-optimization-20261105/function-audit"
INPUTS = json.loads((EVIDENCE / "fp-instruction-optimization-20261105/final/summary.json").read_text())["inputs"]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def union_bytes(ranges):
    merged = []
    for first, last in sorted(ranges):
        if first == last:
            continue
        if merged and first <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], last)
        else:
            merged.append([first, last])
    return sum(last - first for first, last in merged)


class Elf:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        header = struct.unpack_from("<16sHHIQQQIHHHHHH", self.data)
        self.loads = [struct.unpack_from("<IIQQQQQQ", self.data, header[5] + i * header[9])
                      for i in range(header[10])]
        sections = [struct.unpack_from("<IIQQQQIIQQ", self.data, header[6] + i * header[11])
                    for i in range(header[12])]
        self.symbols = []
        for section in sections:
            if section[1] != 2:
                continue
            strings = sections[section[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for at in range(section[4], section[4] + section[5], section[9]):
                name, info, _, index, pc, size = struct.unpack_from("<IBBHQQ", self.data, at)
                if info & 15 == 2 and index and size:
                    self.symbols.append((pc, pc + size, names[name:].split(b"\0", 1)[0].decode(errors="replace")))
        self.decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.decoded = {}
        self.sizes = {}

    def symbol(self, pc):
        found = [s for s in self.symbols if s[0] <= pc < s[1]]
        return min(found, key=lambda s: s[1] - s[0])[2] if found else None

    def memory(self, pc, end):
        for kind, _, offset, address, _, filesz, memsz, _ in self.loads:
            if kind == 1 and address <= pc <= end <= address + memsz:
                first, last = min(pc - address, filesz), min(end - address, filesz)
                return self.data[offset + first:offset + last] + bytes((end - pc) - (last - first))
        raise ValueError(f"range outside ELF PT_LOAD: {pc:x}..{end:x}")

    def instruction(self, pc):
        op = next(self.decoder.disasm(self.memory(pc, pc + 15), pc))
        return {"pc": pc, "pc_hex": hex(pc), "mnemonic": op.mnemonic,
                "operands": op.op_str, "bytes": op.bytes.hex(), "size": op.size,
                "symbol": self.symbol(pc)}

    def block(self, row):
        key = (row["pc"], row["end"], row["count"])
        if key not in self.decoded:
            ops = list(self.decoder.disasm(self.memory(key[0], key[1]), key[0]))
            assert len(ops) == key[2], (key, len(ops))
            assert (ops[-1].address + ops[-1].size if ops else key[0]) == key[1], key
            for op in ops:
                assert self.sizes.setdefault(op.address, op.size) == op.size
            self.decoded[key] = [op.address for op in ops]
        return self.decoded[key]


def trace(name):
    requests, events, aggregate = [], [], None
    for line in (HERE / name).read_text().splitlines():
        try:
            row = json.loads(line)
        except (ValueError, TypeError):
            continue
        event = row.get("function_audit")
        if not event:
            if row.get("measurement_kind") == "application_runtime":
                assert aggregate is None
                aggregate = row
            continue
        if event == "begin":
            requests.append({"begin": row, "block": [], "entry": [], "stop_ready": []})
        assert requests
        request = requests[-1]
        events.append((request["begin"]["seq"], row))
        if event == "begin":
            continue
        if event in ("block", "entry", "stop_ready"):
            request[event].append(row)
        else:
            assert event not in request, (request["begin"], event)
            request[event] = row
    assert aggregate is not None
    assert [r["begin"]["seq"] for r in requests] == list(range(1, len(requests) + 1))
    assert len(requests) == aggregate["compile_attempts"]
    return requests, events, aggregate


def failure_stage(request):
    if request.get("formation", {}).get("error", 0):
        return "formation"
    for event, stage in [("front", "frontend_emission"), ("optimize", "optimizer"), ("backend", "backend")]:
        if event in request and not request[event]["ok"]:
            return stage
    return None


def failed_attempts(requests, elf):
    result = []
    for request in requests:
        stage = failure_stage(request)
        if stage is None:
            continue
        begin, front = request["begin"], request["front"]
        formation = request.get("formation", {})
        prefix = request.get("rejected_prefix", {})
        seq, pc = begin["seq"], front["error_pc"]
        source = next((b for b in request["block"] if b["pc"] <= pc < b["end"]), None)
        fallback = requests[seq]
        assert fallback["begin"]["scope"] == "block" and fallback["begin"]["pc"] == begin["pc"]
        assert "ready" in fallback
        row = {"seq": seq, "root_pc": begin["pc"], "root_hex": hex(begin["pc"]),
               "root_symbol": elf.symbol(begin["pc"]), "fp_mode": begin["fp_mode"],
               "stage": stage, "formation_error": formation.get("error", 0),
               "front_error": front["error"], "ir_error": front["ir_error"],
               "error_pc": pc, "error_hex": hex(pc), "error_instruction": elf.instruction(pc),
               "source_block": source, "error_at_source_block_start": bool(source and source["pc"] == pc),
               "error_at_request_root": pc == begin["pc"],
               "formation_instructions": formation.get("instructions", 0),
               "formation_bytes": formation.get("guest_bytes", 0),
               "formation_decode_calls": formation.get("decode_calls", 0),
               "formation_splits": formation.get("splits", 0),
               "emitted_prefix_instructions": front["guest_instructions"],
               "emitted_prefix_bytes": front["guest_bytes"],
               "born_prefix_nodes": prefix.get("nodes", 0), "born_prefix_words": None,
               "distorm_opcode": prefix.get("opcode") if prefix.get("opcode_available") else None,
               "fallback_seq": fallback["begin"]["seq"],
               "fallback_instructions": fallback["front"]["guest_instructions"]}
        if stage == "formation":
            assert not prefix and front["guest_instructions"] == 0 and not front["nodes_available"]
            row["formation_error_name"] = {1: "InvalidInput", 2: "InvalidEncoding", 3: "Truncated", 4: "OverlappingTarget"}[formation["error"]]
        else:
            assert prefix["guest_instructions"] == front["guest_instructions"]
            assert prefix["pc"] == pc and prefix["words"] is None
        result.append(row)
    return result


def aggregate_closure(requests, aggregate):
    fields = ["compile_attempts", "function_attempts", "block_attempts", "fallbacks", "code_units_published",
              "emitted_guest_instructions", "emitted_guest_bytes", "ir_nodes", "operand_words"]
    assert sum(r["front"]["guest_instructions"] for r in requests) == aggregate["emitted_guest_instructions"]
    assert sum(r["front"]["guest_bytes"] for r in requests) == aggregate["emitted_guest_bytes"]
    assert sum(r["front"]["nodes"] for r in requests if r["front"]["nodes_available"]) == aggregate["ir_nodes"]
    assert sum(r["front"]["words"] for r in requests if r["front"]["nodes_available"]) == aggregate["operand_words"]
    assert sum("publish" in r for r in requests) == aggregate["code_units_published"]
    return {k: aggregate[k] for k in fields}


def fallback_summary(requests, aggregate, elf):
    failures = failed_attempts(requests, elf)
    groups = defaultdict(list)
    for f in failures:
        groups[(f["stage"], f["error_pc"], f.get("formation_error_name"), f["error_instruction"]["mnemonic"])].append(f)
    rows = []
    for (stage, pc, name, mnemonic), group in groups.items():
        rows.append({"stage": stage, "error_pc": pc, "error_hex": hex(pc), "formation_error_name": name,
                     "opcode": mnemonic, "symbol": elf.symbol(pc), "count": len(group),
                     "sequences": [f["seq"] for f in group],
                     "root_pcs": [f["root_pc"] for f in group],
                     "formation_instructions": sum(f["formation_instructions"] for f in group),
                     "formation_bytes": sum(f["formation_bytes"] for f in group),
                     "formation_decode_calls": sum(f["formation_decode_calls"] for f in group),
                     "prefix_instructions": sum(f["emitted_prefix_instructions"] for f in group),
                     "prefix_nodes": sum(f["born_prefix_nodes"] for f in group)})
    return {"aggregate_closure": aggregate_closure(requests, aggregate),
            "failure_stage_counts": dict(Counter(f["stage"] for f in failures)),
            "failure_groups": sorted(rows, key=lambda f: -f["count"]), "failures": failures,
            "failed_formation_decoded_instructions": sum(f["formation_instructions"] for f in failures if f["stage"] == "formation"),
            "failed_formation_decoded_bytes": sum(f["formation_bytes"] for f in failures if f["stage"] == "formation"),
            "failed_formation_decode_calls": sum(f["formation_decode_calls"] for f in failures if f["stage"] == "formation"),
            "failed_emission_prefix_instructions": sum(f["emitted_prefix_instructions"] for f in failures),
            "failed_emission_prefix_nodes": sum(f["born_prefix_nodes"] for f in failures),
            "actual_born_nodes_including_rejected_prefix": aggregate["ir_nodes"] + sum(f["born_prefix_nodes"] for f in failures),
            "failed_prefix_operand_words": None, "timing_used_for_qor": False}


def coremark():
    requests, events, aggregate = trace("coremark20-function.stderr")
    elf = Elf(INPUTS["coremark"]["path"])
    assert sha(elf.path) == INPUTS["coremark"]["sha256"]
    expected = {"compile_attempts": 686, "function_attempts": 681, "block_attempts": 5,
                "fallbacks": 5, "code_units_published": 681, "emitted_guest_instructions": 16584,
                "ir_nodes": 50591, "operand_words": 84624}
    assert all(aggregate[k] == v for k, v in expected.items())
    failures = failed_attempts(requests, elf)
    assert len(failures) == 5 and all(f["stage"] == "frontend_emission" for f in failures)
    successes = [r for r in requests if "ready" in r]
    assert len(successes) == 681
    counts, ranges = defaultdict(Counter), defaultdict(list)
    function_counts, function_ranges = defaultdict(Counter), defaultdict(list)
    unit_rows, slices, pc_owners = [], Counter(), defaultdict(list)
    prior_coverage = defaultdict(dict)
    for request in requests:
        for b in request["block"]:
            elf.block(b)
        if "ready" not in request:
            continue
        begin, front = request["begin"], request["front"]
        seq, mode, root = begin["seq"], begin["fp_mode"], begin["pc"]
        blocks = request["block"]
        if begin["scope"] == "block":
            assert not blocks
            blocks = [{"pc": root, "end": root + front["guest_bytes"], "count": front["guest_instructions"]}]
        pcs = [pc for block in blocks for pc in elf.block(block)]
        assert len(pcs) == front["guest_instructions"] and len(pcs) == len(set(pcs))
        assert sum(b["end"] - b["pc"] for b in blocks) == front["guest_bytes"]
        repeated = sum(pc in counts[mode] for pc in pcs)
        row = {"seq": seq, "root_pc": root, "root_hex": hex(root), "symbol": elf.symbol(root),
               "scope": begin["scope"], "fp_mode": mode, "source_blocks": len(blocks),
               "nonempty_source_blocks": sum(b["count"] > 0 for b in blocks),
               "guest_instructions": len(pcs), "guest_bytes": front["guest_bytes"],
               "new_instructions": len(pcs) - repeated, "repeated_instructions": repeated,
               "entry_previously_in_source_coverage": root in prior_coverage[mode],
               "prior_source_owner_seq": prior_coverage[mode].get(root),
               "all_coverage_previously_present": bool(pcs) and repeated == len(pcs),
               "nodes": front["nodes"], "words": front["words"],
               "native_bytes": request["publish"]["native_bytes"],
               "owner_base": request["publish"]["owner_base"],
               "offered_extra_entries": len(request["entry"]), "stop_ready_events": len(request["stop_ready"])}
        unit_rows.append(row)
        counts[mode].update(pcs)
        ranges[mode].extend((b["pc"], b["end"]) for b in blocks)
        for pc in pcs:
            prior_coverage[mode].setdefault(pc, seq)
            pc_owners[(mode, pc)].append(seq)
        for b in blocks:
            slices[(mode, b["pc"], b["end"], b["count"])] += 1
        if begin["scope"] == "function":
            function_counts[mode].update(pcs)
            function_ranges[mode].extend((b["pc"], b["end"]) for b in blocks)

    # The existing log is ordered and single-threaded. InsertReady registers
    # root and extras between publish and entry events, before ready is logged.
    by_seq = {r["begin"]["seq"]: r for r in requests}
    owners, ready, first_ready = {}, {}, []
    accepted, retained_prior, stops = [], [], []
    ready_counts_by_mode = defaultdict(Counter)
    for seq, event in events:
        kind = event["function_audit"]
        request = by_seq[seq]
        mode = request["begin"]["fp_mode"]
        if kind == "publish":
            base, root = event["owner_base"], event["root_pc"]
            assert base not in owners
            owners[base] = {"seq": seq, "fp_mode": mode, "root_pc": root, "native_bytes": event["native_bytes"]}
            key = mode, root
            assert key not in ready
            ready[key] = {"entry": base, "owner_base": base, "first_seq": seq, "kind": "root"}
            ready_counts_by_mode[mode]["root"] += 1
            first_ready.append({"fp_mode": mode, "pc": root, **ready[key]})
        elif kind == "entry":
            owner = owners[event["owner_base"]]
            assert owner["seq"] == seq and owner["root_pc"] == event["root_pc"] and owner["fp_mode"] == mode
            assert 0 < event["word"] * 4 < owner["native_bytes"]
            actual_owner = owners[event["actual_owner_base"]]
            assert actual_owner["fp_mode"] == mode
            actual_offset = event["actual_entry"] - event["actual_owner_base"]
            assert 0 <= actual_offset < actual_owner["native_bytes"] and actual_offset % 4 == 0
            assert event["accepted_here"] == (event["actual_entry"] == event["owner_base"] + event["word"] * 4)
            key = mode, event["pc"]
            if key in ready:
                old = ready[key]
                assert event["actual_entry"] == old["entry"] and event["actual_owner_base"] == old["owner_base"]
                retained_prior.append({"seq": seq, "pc": key[1], "first_seq": old["first_seq"]})
            else:
                assert event["accepted_here"] and actual_owner["seq"] == seq
                ready[key] = {"entry": event["actual_entry"], "owner_base": event["actual_owner_base"],
                              "first_seq": seq, "kind": "extra"}
                ready_counts_by_mode[mode]["extra"] += 1
                first_ready.append({"fp_mode": mode, "pc": key[1], **ready[key]})
            accepted.append(event)
        elif kind == "stop_ready":
            key = mode, event["pc"]
            actual = ready[key]
            assert actual["first_seq"] < seq
            assert (actual["entry"], actual["owner_base"]) == (event["entry"], event["owner_base"])
            stops.append({"request_seq": seq, "request_root": request["begin"]["pc"], "fp_mode": mode,
                          "pc": key[1], "pc_hex": hex(key[1]), "symbol": elf.symbol(key[1]), **actual})
        elif kind == "ready":
            key = mode, event["pc"]
            assert ready[key]["entry"] == event["owner_base"] and ready[key]["kind"] == "root"

    assert aggregate["cache_invalidations"] == 0
    assert len(ready) == aggregate["peak_cache_entries"]
    assert len(accepted) == sum(r["publish"]["extra_entry_count"] for r in successes)
    for request in successes:
        begin = request["begin"]
        extra = {e["pc"] for e in request["entry"]}
        assert len(extra) == len(request["entry"])
        if begin["scope"] == "function":
            nonempty = {b["pc"] for b in request["block"] if b["count"]}
            assert extra == nonempty - {begin["pc"]}, (begin, len(extra), len(nonempty))
        else:
            assert not extra

    def coverage(mode_counts, mode_ranges):
        per_mode = [{"fp_mode": mode, "instruction_occurrences": sum(c.values()),
                     "unique_instructions": len(c), "repeated_instruction_occurrences": sum(c.values()) - len(c),
                     "guest_byte_occurrences": sum(b - a for a, b in mode_ranges[mode]),
                     "unique_guest_bytes": union_bytes(mode_ranges[mode])}
                    for mode, c in sorted(mode_counts.items())]
        all_pcs = set().union(*(set(c) for c in mode_counts.values()))
        return {"per_mode": per_mode, "instruction_occurrences": sum(sum(c.values()) for c in mode_counts.values()),
                "unique_instructions_within_modes": sum(len(c) for c in mode_counts.values()),
                "repeated_instruction_occurrences_within_modes": sum(sum(c.values()) - len(c) for c in mode_counts.values()),
                "guest_byte_occurrences": sum(b - a for r in mode_ranges.values() for a, b in r),
                "unique_guest_bytes_within_modes": sum(union_bytes(r) for r in mode_ranges.values()),
                "cross_mode_instruction_copies": sum(len(c) for c in mode_counts.values()) - len(all_pcs)}

    all_coverage, fn_coverage = coverage(counts, ranges), coverage(function_counts, function_ranges)
    all_coverage["repeated_guest_byte_occurrences_within_modes"] = all_coverage["guest_byte_occurrences"] - all_coverage["unique_guest_bytes_within_modes"]
    all_coverage["entries_already_in_prior_source_coverage"] = sum(r["entry_previously_in_source_coverage"] for r in unit_rows)
    all_coverage["units_entirely_covered_by_prior_units"] = sum(r["all_coverage_previously_present"] for r in unit_rows)
    all_coverage["exact_repeated_source_slice_occurrences"] = sum(v - 1 for v in slices.values())
    all_coverage["source_blocks_in_successful_function_units"] = sum(len(r["block"]) for r in successes if r["begin"]["scope"] == "function")
    cap_fields = ["ir_reuse_reserved_bytes", "word_capacity_bytes", "link_capacity_bytes", "entry_capacity_bytes"]
    capacities = {k: max(r["publish"][k] for r in successes) for k in cap_fields}
    capacities["max_combined_retained_bytes"] = max(sum(r["publish"][k] for k in cap_fields) for r in successes)
    capacities["last_publish"] = {k: successes[-1]["publish"][k] for k in cap_fields}
    capacities["post_registration_last_entries_capacity_bytes"] = successes[-1]["ready"]["entries_capacity_bytes"]
    capacities["high_water_sequences"] = {k: next(r["begin"]["seq"] for r in successes if r["publish"][k] == capacities[k]) for k in cap_fields}
    stop_counts = Counter((r["fp_mode"], r["pc"]) for r in stops)
    owner_counts = Counter(e["owner_base"] for e in first_ready)
    registration = {"published_native_owners": len(owners), "root_entries": len(successes),
                    "offered_extra_entries": len(accepted), "offers_accepted_here": sum(e["accepted_here"] for e in accepted),
                    "offers_retaining_prior_ready_entry": len(retained_prior),
                    "unique_ready_entries": len(ready), "extra_entries_first_ready_here": len(ready) - len(successes),
                    "per_mode": [{"fp_mode": m, **dict(c)} for m, c in sorted(ready_counts_by_mode.items())],
                    "first_ready_contract_validated": True, "native_owner_ranges_validated": True,
                    "all_extra_entries_match_nonempty_source_block_starts": True,
                    "empty_source_blocks_registered": 0, "non_source_exit_stub_entries_registered": 0,
                    "max_entries_sharing_one_native_owner": max(owner_counts.values()),
                    "top_native_owners_by_ready_entries": [{**owners[base], "root_hex": hex(owners[base]["root_pc"]),
                                                           "symbol": elf.symbol(owners[base]["root_pc"]), "ready_entries": n}
                                                          for base, n in owner_counts.most_common(8)]}
    stop_result = {"actual_positive_lookup_events": len(stops), "unique_mode_pc": len(stop_counts),
                   "attempts_with_positive_lookup": len({s["request_seq"] for s in stops}),
                   "lookups_of_extra_entries": sum(s["kind"] == "extra" for s in stops),
                   "lookups_of_root_entries": sum(s["kind"] == "root" for s in stops),
                   "all_match_previously_ready_actual_entry_and_owner": True,
                   "top_pcs": [{"fp_mode": mode, "pc": pc, "pc_hex": hex(pc), "symbol": elf.symbol(pc),
                                "events": n, "first_ready_seq": ready[(mode, pc)]["first_seq"],
                                "entry_kind": ready[(mode, pc)]["kind"]}
                               for (mode, pc), n in stop_counts.most_common(12)],
                   "note": "Positive lookup events are actual callback invocations, not unique pruned descendants or saved instruction counts."}
    baseline = json.loads((BASE / "analysis.json").read_text())
    before = baseline["successful_coverage"]
    comparison_fields = ["instruction_occurrences", "unique_instructions_within_modes", "repeated_instruction_occurrences_within_modes",
                         "guest_byte_occurrences", "unique_guest_bytes_within_modes", "repeated_guest_byte_occurrences_within_modes"]
    comparison = {k: {"baseline": before[k], "a3": all_coverage[k], "delta": all_coverage[k] - before[k]}
                  for k in comparison_fields}
    comparison["attempts"] = {"baseline": 705, "a3": len(requests), "delta": len(requests) - 705}
    comparison["published_units"] = {"baseline": 700, "a3": len(successes), "delta": len(successes) - 700}
    comparison["failed_prefix_nodes"] = {"baseline": baseline["work_boundaries"]["rejected_prefix_nodes_excluded"],
                                         "a3": sum(f["born_prefix_nodes"] for f in failures)}
    stdout = (HERE / "coremark20-function.stdout").read_text()
    crc = re.search(r"crcfinal\s*:\s*(0x[0-9a-fA-F]+)", stdout).group(1)
    assert aggregate["runtime_completed"] and aggregate["exit_code"] == 0 and int(crc, 16) == 0x4983
    prefix_n = sum(f["born_prefix_nodes"] for f in failures)
    prefix_i = sum(f["emitted_prefix_instructions"] for f in failures)
    assert prefix_n == 265 and prefix_i == 169
    comparison["all_actual_born_nodes"] = {"baseline": baseline["work_boundaries"]["all_actual_born_nodes_including_rejected_prefix"], "a3": aggregate["ir_nodes"] + prefix_n, "delta": aggregate["ir_nodes"] + prefix_n - baseline["work_boundaries"]["all_actual_born_nodes_including_rejected_prefix"]}
    result = {"kind": "A3_final_cold_function_coverage_entry_owner_audit_no_qor_timing",
              "inputs": {"production_source_digest": "a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52",
                         "production_source_manifest_sha256": sha(HERE.parent / "attempt3/source-manifest.json"),
                         "diagnostic_binary_sha256": json.loads((HERE / "receipt.json").read_text())["binary_sha256"],
                         "trace_sha256": sha(HERE / "coremark20-function.stderr"),
                         "stdout_sha256": sha(HERE / "coremark20-function.stdout"),
                         "execution_sha256": sha(HERE / "execution.json"),
                         "guest_elf": str(elf.path), "guest_elf_sha256": sha(elf.path),
                         "baseline_analysis_sha256": sha(BASE / "analysis.json"),
                         "baseline_production_source_digest": baseline["inputs"]["production_source_digest"]},
              "oracle": {"runtime_completed": True, "exit_code": 0, "crcfinal": crc,
                         "short_run_errors_detected_expected": "Errors detected" in stdout,
                         "timing_used_for_qor": False, "reexecuted_for_host_assertion": False},
              "aggregate_closure": {**aggregate_closure(requests, aggregate),
                                    "successful_function_units": sum(r["begin"]["scope"] == "function" for r in successes),
                                    "successful_block_fallback_units": sum(r["begin"]["scope"] == "block" for r in successes),
                                    "failure_stage_counts": dict(Counter(f["stage"] for f in failures)),
                                    "cache_invalidations": aggregate["cache_invalidations"],
                                    "peak_cache_entries": aggregate["peak_cache_entries"]},
              "boundary_validation": {"decoder": "Capstone " + capstone.__version__,
                                      "unique_ranges_checked": len(elf.decoded),
                                      "all_logged_counts_and_endpoints_match": True,
                                      "within_successful_unit_duplicate_instruction_occurrences": 0},
              "successful_coverage": all_coverage, "successful_function_only_coverage": fn_coverage,
              "failed_attempts": failures,
              "work_boundaries": {"accepted_front_program_nodes": aggregate["ir_nodes"], "rejected_prefix_nodes": prefix_n,
                                  "all_actual_born_nodes": aggregate["ir_nodes"] + prefix_n,
                                  "accepted_front_operand_words": aggregate["operand_words"], "rejected_prefix_operand_words": None,
                                  "emitted_prefix_instructions_in_runtime_aggregate": prefix_i,
                                  "successful_unit_instruction_occurrences": all_coverage["instruction_occurrences"],
                                  "decoded_source_instructions_in_failed_front_attempts": sum(f["formation_instructions"] for f in failures),
                                  "decoded_bytes_in_failed_front_attempts": sum(f["formation_bytes"] for f in failures),
                                  "byte_decoder_calls": aggregate["compile_decode_calls"]},
              "entry_registration": registration, "stop_ready": stop_result,
              "retained_capacity_bytes": capacities, "baseline_comparison": comparison,
              "largest_remaining_repeated_units": sorted(unit_rows, key=lambda x: x["repeated_instructions"], reverse=True)[:8],
              "limitations": [
                  "Compilation coverage and registration counts are not dynamic retired instruction counts or QoR timing.",
                  "All FP modes are compared separately; cross-mode copies are not treated as removable duplication.",
                  "A3 ordinary run and this one diagnostic run have slightly different printf-path formation; only this trace's aggregates are combined.",
                  "Source bytes and instruction counts are validated against immutable ELF; no captured RX equality is claimed.",
                  "Failed-prefix operand words are unavailable; no CountWork or additional IR scan was introduced.",
                  "A3 source-B entries and stop-ready remain enabled; speculative CALL continuations and the FC_INT exception are absent."]}
    (HERE / "analysis.json").write_text(json.dumps(result, indent=2) + "\n")
    (HERE / "unit-coverage.jsonl").write_text("".join(json.dumps(r, separators=(",", ":")) + "\n" for r in unit_rows))
    lines = ["# A3 final cold coverage and entry-owner audit", "", "This is the single existing CoreMark20 diagnostic, not a timing baseline. Exit0 and CRC4983 passed; the short-run validation warning is expected.", "",
             "| Actual diagnostic work | Old baseline | A3 |", "|---|---:|---:|",
             f"| Attempts / published native owners | 705 / 700 | {len(requests)} / {len(successes)} |",
             f"| Successful guest instruction occurrences | {before['instruction_occurrences']:,} | {all_coverage['instruction_occurrences']:,} |",
             f"| Unique guest instructions (separate FP modes) | {before['unique_instructions_within_modes']:,} | {all_coverage['unique_instructions_within_modes']:,} |",
             f"| Repeated instruction occurrences | {before['repeated_instruction_occurrences_within_modes']:,} | {all_coverage['repeated_instruction_occurrences_within_modes']:,} |",
             f"| Successful guest byte occurrences / union | {before['guest_byte_occurrences']:,} / {before['unique_guest_bytes_within_modes']:,} | {all_coverage['guest_byte_occurrences']:,} / {all_coverage['unique_guest_bytes_within_modes']:,} |",
             f"| Accepted front nodes / failed-prefix nodes | 87,595 / 251 | {aggregate['ir_nodes']:,} / {prefix_n:,} |",
             f"| All actual born nodes, including failures | 87,846 | {aggregate['ir_nodes'] + prefix_n:,} |", "",
             f"686 attempts = 681 Function attempts + 5 Block retries. 676 Function units and 5 Block units publish. The 5 frontend failures add {prefix_i} emitted-prefix instructions and {prefix_n} born nodes; all actual born IR is {aggregate['ir_nodes'] + prefix_n:,}. Formation, optimizer and backend failures are zero. Runtime emitted {aggregate['emitted_guest_instructions']:,} instructions includes rejected prefixes; successful-unit coverage is {all_coverage['instruction_occurrences']:,}.", "",
             f"Actual registration: {len(successes)} native owners, {len(accepted):,} extra source-B entry offers, {len(ready):,} unique ready PCs. Every offered extra was accepted here; no duplicate offer retained an older owner in this run. Root plus extras share the actual native owner range; every extra is a nonempty source-B start, and every such start except the root is offered. No empty B or exit stub is registered.", "",
             f"stop-ready recorded {len(stops)} positive callback events across {len(stop_counts)} mode/PC keys and {len({s['request_seq'] for s in stops})} attempts. {sum(s['kind']=='extra' for s in stops)} refer to extra entries and {sum(s['kind']=='root' for s in stops)} to roots. Every address/owner matches an earlier ready registration. Callback counts are not a count of all pruned descendants or saved instructions.", "",
             f"Both actual coverage sets are mode0: no legitimate cross-FP-mode copies are charged as duplication. A3 removes {before['repeated_instruction_occurrences_within_modes'] - all_coverage['repeated_instruction_occurrences_within_modes']:,} repeated occurrences and {baseline['work_boundaries']['all_actual_born_nodes_including_rejected_prefix'] - aggregate['ir_nodes'] - prefix_n:,} actual born nodes. Unique coverage differs by {all_coverage['unique_instructions_within_modes'] - before['unique_instructions_within_modes']}; the full total-work difference is not claimed to come only from reuse. No A3 unit root is inside earlier source coverage and no A3 unit is entirely covered by earlier units; the remaining 191 repeated instructions are retained, not declared eliminated.", "",
             f"Retained high water: IR {capacities['ir_reuse_reserved_bytes']:,} B, words {capacities['word_capacity_bytes']:,} B, links {capacities['link_capacity_bytes']:,} B, entries {capacities['entry_capacity_bytes']:,} B; maximum simultaneous {capacities['max_combined_retained_bytes']:,} B. Entry scratch capacity is charged and included.", "",
             "All coverage is mode-separated; this run's mode set and cross-mode copies are recorded in JSON. Counts and endpoints of every actual source range match the fixed ELF. The trace does not prove a runtime speedup, and ordinary A3 statistics are not spliced into this diagnostic. A3 retains source-B entry registration and stop-ready while leaving CALL continuations for their first actual request. A2 speculative-continuation failures remain separately archived; no A2 run data is combined here."]
    (HERE / "report.md").write_text("\n".join(lines) + "\n")
    return result


if __name__ == "__main__":
    execution = json.loads((HERE / "execution.json").read_text())
    receipt_in = json.loads((HERE / "receipt.json").read_text())
    production = json.loads((HERE.parent / "attempt3/source-manifest.json").read_text())
    assert execution["source_digest"] == receipt_in["production_source_digest"] == production["digest"] == "a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52"
    assert execution["runner_sha256"] == receipt_in["binary_sha256"] == "218342a432565743e40243ddc478a25c4e1a0920c79d1873854de02ff510e8ce"
    assert execution["stderr_sha256"] == sha(HERE / "coremark20-function.stderr")
    assert execution["stdout_sha256"] == sha(HERE / "coremark20-function.stdout")
    assert execution["diagnostic_not_qor"] is True
    c = coremark()
    files = ["analyze.py", "analysis.json", "unit-coverage.jsonl", "report.md"]
    receipt = {"kind": "offline_existing_A3_trace_and_ELF_analysis_only", "production_modified": False,
               "built_or_executed": False, "timing_used_for_qor": False,
               "production_source_digest": production["digest"],
               "diagnostic_binary_sha256": receipt_in["binary_sha256"],
               "trace_sha256": execution["stderr_sha256"],
               "files": {name: sha(HERE / name) for name in files}}
    (HERE / "analysis-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({k: c[k] for k in ["aggregate_closure", "successful_coverage", "work_boundaries", "entry_registration", "stop_ready", "retained_capacity_bytes", "baseline_comparison"]}, indent=2))
