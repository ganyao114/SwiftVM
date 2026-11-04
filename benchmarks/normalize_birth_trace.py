#!/usr/bin/env python3
"""Offline original-IR birth input -> diagnostic SVMIR_REPLAY_1 writer input.

This is not a frontend, optimizer, executable-IR conversion, or compile budget
measurement. BODY_ONLY retains every birth, not the final edited program.
The raw JSONL and the bound JSON sidecar remain necessary evidence.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile


INVALID = (1 << 32) - 1
COHORT = "8cda2889f05a96fb3a12c021013d18c05ea4dca0fbdb2a63f81f8318364dd23e"
OLD_TYPES = "VOID U8 U16 U32 U64 S8 S16 S32 S64 V8 V16 V32 V64 V128 V256".split()
NEW_TYPES = {name: ("Void" if name == "VOID" else name) for name in OLD_TYPES}
TRAITS = "uniform abi host_fp host_registers guest_state fault reentry host_flags".split()
RECORD = {"node", "origin", "legacy_id", "block", "opcode", "opcode_name",
          "natural_type", "type_name", "flags", "slots"}
COMMON = {"seq", "event", "request"}
EVENT_FIELDS = {
    "request_begin": {"scope", "pc"}, "block": {"block", "location", "function_owner"},
    "birth": RECORD | {"placement"}, "mutation": RECORD,
    "republish": RECORD | {"placement"}, "retire": {"node"},
    "unlink": {"node", "block"}, "move_before": {"node", "block", "relative"},
    "block_retire": {"block"}, "terminal": {"block", "terminal"},
    "block_frontier": {"block", "order", "terminal"},
    "request_front_finish": {"births", "mutations", "blocks", "missing_source_observations"},
    "request_end": {"front_finished", "outcome", "reason", "births", "mutations", "blocks"},
}


def need(condition, message):
    if not condition:
        raise ValueError(message)


def fields(value, required, optional=()):
    need(isinstance(value, dict) and required <= value.keys() and
         value.keys() <= required | set(optional), "missing/unknown fields: " + repr(value))


def number(value, bits=64):
    if isinstance(value, str):
        need(bool(re.fullmatch(r"(?:0x[0-9a-fA-F]+|[0-9]+)", value)), "invalid unsigned number")
        value = int(value, 16 if value.startswith("0x") else 10)
    need(type(value) is int and 0 <= value < (1 << bits), "unsigned integer overflow/type")
    return value


def boolean(value):
    need(type(value) is bool, "expected explicit boolean")
    return value


def old_type(value, name=None):
    value = number(value, 8)
    need(value < len(OLD_TYPES), "unknown original type")
    if name is not None:
        need(name == OLD_TYPES[value], "original type/name mismatch")
    return NEW_TYPES[OLD_TYPES[value]]


def integer(type_name):
    return type_name in "Bool U8 U16 U32 U64 S8 S16 S32 S64".split()


def literal(bits, type_name):
    width = {"Bool": 1, "U8": 8, "S8": 8, "V8": 8, "U16": 16, "S16": 16,
             "V16": 16, "U32": 32, "S32": 32, "V32": 32, "U64": 64,
             "S64": 64, "V64": 64}.get(type_name)
    need(width is not None, "literal has no lossless new 64-bit representation: " + type_name)
    mask = (1 << width) - 1
    need(bits <= mask or (type_name.startswith("S") and bits & ~mask == ((1 << 64) - 1) & ~mask
                        and bits & (1 << (width - 1))), "literal exceeds declared width")


def declarations(path, new):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
    rows = []
    for match in re.finditer(r"\bINST\s*\(", text):
        depth, start, parts = 0, match.end(), []
        for at in range(start, len(text)):
            char = text[at]
            if char == "(":
                depth += 1
            elif char == ")":
                if depth == 0:
                    parts.append(text[start:at].strip())
                    break
                depth -= 1
            elif char == "," and depth == 0:
                parts.append(text[start:at].strip())
                start = at + 1
        need(len(parts) >= (4 if new else 3), "invalid opcode declaration")
        name, ret, policy = parts[:3]
        rows.append((name, ret, policy, parts[4:] if new else parts[3:]))
    need(rows and len({x[0] for x in rows}) == len(rows), "missing/duplicate opcodes")
    return rows


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            result.update(chunk)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": result.hexdigest()}


def atom(kind, type_name="Void", payload=0, count=0, code=0):
    return (kind, type_name, payload, count, code)


class Normalizer:
    def __init__(self, args):
        self.args = args
        self.old_rows = declarations(args.old_source / "source/runtime/ir/ir.inc", False)
        self.schemas = {row[0]: row for row in declarations(args.new_source / "source/runtime/ir/opcodes.inc", True)}
        for row in self.old_rows:
            need(row[0] in self.schemas and self.schemas[row[0]] == row,
                 "original/new signature differs: " + row[0])
        self.events, self.rules = Counter(), Counter()
        self.queries, self.requests, self.terminals = [], [], []
        self.effects, self.effects_uses = {}, defaultdict(list)
        self.excluded = Counter()
        self.header = self.footer = None
        self.atoms = self.nodes = self.uses = self.words = self.emitted_events = 0
        self.context = ""

    def origin(self, value):
        fields(value, {"store", "session", "node"})
        return (number(value["store"]), number(value["session"]), number(value["node"], 32))

    def scalar(self, value, request, local, natural, current, seq):
        kind = value.get("kind")
        if kind == "Value":
            fields(value, {"kind", "view_type", "view_name", "effective_type", "node", "source_alive"})
            source = number(value["node"])
            need(source in local and source in current and boolean(value["source_alive"]),
                 "unborn/dead/cross-owner source")
            view = old_type(value["view_type"], value["view_name"])
            effective = old_type(value["effective_type"])
            need(effective != "Void" and natural[source] != "Void", "Void source value")
            need(effective == (current[source] if view == "Void" else view), "original effective type mismatch")
            if view == "Void" and natural[source] != effective:
                view = effective
                self.rules["explicit_effective_view_preserves_old_width"] += 1
            return atom(1, view, local[source]), effective
        if kind == "Imm":
            fields(value, {"kind", "type", "type_name", "bits"})
            type_name, bits = old_type(value["type"], value["type_name"]), number(value["bits"])
            literal(bits, type_name)
            return atom(2, type_name, bits), type_name
        raise ValueError("expected Value/Imm, not " + str(kind))

    def argument(self, value, request, local, natural, current, seq, slot):
        kind = value.get("kind")
        if kind in ("Value", "Imm"):
            cell, type_name = self.scalar(value, request, local, natural, current, seq)
            return [cell], type_name
        if kind in ("Uniform", "Local"):
            key = "offset" if kind == "Uniform" else "id"
            fields(value, {"kind", key, "type", "type_name"})
            type_name = old_type(value["type"], value["type_name"])
            need(type_name != "Void", "Void uniform/local")
            return [atom(3 if kind == "Uniform" else 4, type_name, number(value[key], 32))], type_name
        if kind == "Cond":
            fields(value, {"kind", "condition"})
            condition = number(value["condition"], 8)
            need(condition < 16, "condition outside original enum")
            return [atom(5, payload=condition)], "Void"
        if kind == "Flags":
            fields(value, {"kind", "mask"})
            return [atom(6, payload=number(value["mask"]))], "Void"
        if kind == "Params":
            fields(value, {"kind", "count", "items"})
            need(isinstance(value["items"], list) and number(value["count"], 32) == len(value["items"]),
                 "Params missing items")
            cells = [atom(8, count=len(value["items"]))]
            for item in value["items"]:
                cells.append(self.scalar(item, request, local, natural, current, seq)[0])
            return cells, "Void"
        if kind == "Lambda":
            fields(value, {"kind", "target", "traits", "effect_set"})
            fields(value["traits"], set(TRAITS))
            traits = [number(value["traits"][key], 8) for key in TRAITS]
            effect = value["effect_set"]
            need(effect is None or isinstance(effect, list), "invalid uniform effect dictionary")
            if effect is not None:
                for item in effect:
                    fields(item, {"offset", "size"})
                    offset, size = number(item["offset"], 32), number(item["size"], 32)
                    need(size and offset + size <= (1 << 32), "invalid uniform effect range")
            need(traits[0] == 0 or effect is not None, "missing non-Unknown uniform effect dictionary")
            key = str(traits[0])
            need(key not in self.effects or self.effects[key] == effect, "source uniform dictionary changed")
            self.effects[key] = effect
            self.effects_uses[key].append({"request": request, "seq": seq, "slot": slot})
            target, type_name = self.scalar(value["target"], request, local, natural, current, seq)
            need(integer(type_name), "noninteger Lambda target")
            return [atom(9, payload=sum(t << (8 * i) for i, t in enumerate(traits))), target], type_name
        raise ValueError("unmapped argument kind: " + str(kind))

    def record(self, event, request, local, natural, current):
        index = number(event["opcode"], 16)
        need(1 <= index <= len(self.old_rows), "unknown original opcode")
        name, ret, policy, signatures = self.old_rows[index - 1]
        need(name == event["opcode_name"], "original opcode/name mismatch")
        original = old_type(event["natural_type"], event["type_name"])
        flags = event["flags"]
        fields(flags, {"mask", "profile", "branch_only"})
        mask, profile, branch = number(flags["mask"]), number(flags["profile"], 8), boolean(flags["branch_only"])
        need(profile <= 3, "invalid flag profile")
        need(isinstance(event["slots"], list), "missing physical slots")
        slots = [{"kind": "Void"} for _ in range(5)]
        present = set()
        for entry in event["slots"]:
            fields(entry, {"slot", "arg"})
            slot = number(entry["slot"], 8)
            need(slot < 5 and slot not in present and isinstance(entry["arg"], dict), "bad/duplicate physical slot")
            slots[slot] = entry["arg"]
            present.add(slot)
        cells, first, at, absent = [], "Void", 0, False
        for signature in signatures:
            width = 3 if signature.endswith("Operand") else 1
            need(at + width <= 5, "physical signature exceeds original slots")
            value = slots[at]
            optional = signature.startswith("Optional")
            if value.get("kind") == "Void":
                fields(value, {"kind"})
                need(optional and all(x == {"kind": "Void"} for x in slots[at:at + width]), "missing required argument")
                absent = True
                at += width
                continue
            need(not absent, "optional argument hole")
            if width == 3:
                fields(value, {"kind", "op_type", "shift_ext"})
                need(value["kind"] == "Operand", "missing compound Operand header")
                operation, shift = number(value["op_type"], 8), number(value["shift_ext"], 8)
                left, left_type = self.scalar(slots[at + 1], request, local, natural, current, event["seq"])
                right = slots[at + 2]
                if right.get("kind") == "Void":
                    fields(right, {"kind"})
                    need(operation in (0, 1) and shift == 0, "nonidentity unary expression")
                    part, type_name = [left], left_type
                    self.rules["unary_None_or_defaultPlus_to_direct_atom"] += 1
                else:
                    code = {1: 1, 2: 3, 4: 4, 8: 7}.get(operation)
                    need(code is not None and shift < 64 and (operation == 8 or shift == 0),
                         "unmapped original expression/shift")
                    other, right_type = self.scalar(right, request, local, natural, current, event["seq"])
                    part, type_name = [atom(7, count=2, code=code | (shift << 8)), left, other], left_type
                    if "IntOperand" in signature:
                        need(integer(right_type), "noninteger expression child")
                if "IntOperand" in signature:
                    need(integer(type_name), "noninteger expression")
            else:
                part, type_name = self.argument(value, request, local, natural, current, event["seq"], at)
                expected = {"Value": 1, "IntValue": 1, "Int32Value": 1, "Int64Value": 1,
                            "Imm": 2, "IntImm": 2, "Uniform": 3, "Local": 4, "Cond": 5,
                            "Flags": 6, "Params": 8, "Lambda": 9}.get(signature)
                need((signature == "OptionalData" and part[0][0] in (1, 2)) or
                     (expected is not None and part[0][0] == expected), "argument/signature kind mismatch")
                if signature.startswith("Int"):
                    need(integer(type_name), "noninteger argument")
                    bits = 32 if signature == "Int32Value" else 64 if signature == "Int64Value" else None
                    if bits:
                        need(type_name in ("U" + str(bits), "S" + str(bits)), "integer argument width mismatch")
            if at == 0:
                first = type_name
            cells.extend(part)
            at += width
        need(all(x == {"kind": "Void"} for x in slots[at:]), "extra original physical argument")
        result = "Bool" if ret == "BOOL" else original
        if policy == "Void":
            need(original == "Void", "original Void policy mismatch")
        elif policy.endswith("Fixed"):
            need(original == ("U8" if ret == "BOOL" else ret), "original fixed result mismatch")
        elif policy.endswith("Arg0"):
            need(first != "Void", "original Arg0 has no type source")
            if original != first:
                self.rules["explicit_Arg0_result_preserved_observations"] += 1
        elif policy != "Call":
            need(original != "Void", "original explicit result is Void")
        if policy.startswith("Integer"):
            need(integer(result), "noninteger result")
        return name, result, mask, profile, int(branch), cells

    def terminal(self, value, local, natural, current, seq, request):
        need(isinstance(value, dict), "missing terminal tree")
        kind = value.get("kind")
        if kind in ("Invalid", "ReturnToDispatch", "ReturnToHost", "PopRSBHint"):
            fields(value, {"kind"})
        elif kind in ("LinkBlock", "LinkBlockFast", "ExternalLinkBlock"):
            fields(value, {"kind", "next"})
            number(value["next"])
        elif kind in ("If", "Condition"):
            fields(value, {"kind", "condition", "then", "else"})
            if kind == "If":
                self.scalar(value["condition"], request, local, natural, current, seq)
            else:
                need(number(value["condition"], 8) < 16, "invalid terminal condition")
            self.terminal(value["then"], local, natural, current, seq, request)
            self.terminal(value["else"], local, natural, current, seq, request)
        elif kind == "CheckHalt":
            fields(value, {"kind", "else"})
            self.terminal(value["else"], local, natural, current, seq, request)
        elif kind == "Switch":
            fields(value, {"kind", "value", "cases"})
            self.scalar(value["value"], request, local, natural, current, seq)
            need(isinstance(value["cases"], list), "missing switch cases")
            for case in value["cases"]:
                fields(case, {"case", "then"})
                self.scalar(case["case"], request, local, natural, current, seq)
                self.terminal(case["then"], local, natural, current, seq, request)
        else:
            raise ValueError("unknown source terminal: " + str(kind))

    def write_request(self, events, output):
        begin, end = events[0], events[-1]
        request, scope = begin["request"], begin["scope"]
        need(scope in ("block", "function"), "unknown source scope")
        number(begin["pc"])
        blocks, births, current, natural, origins, groups = {}, [], {}, {}, set(), {}
        local, owner_of, last_record = {}, {}, {}
        terminals, mutation_count, finished, excluded_seqs = [], 0, False, defaultdict(list)
        group_order = []
        for event in events[1:-1]:
            self.context = "request %s seq %s %s" % (request, event["seq"], event["event"])
            kind = event["event"]
            need(not finished, "frontend event after front finish")
            if kind == "block":
                block = number(event["block"])
                need(block == len(blocks), "source block IDs are not dense declarations")
                owner = event["function_owner"]
                if owner is not None:
                    number(owner)
                need((scope == "block") == (owner is None), "scope/function owner mismatch")
                if owner not in groups:
                    groups[owner] = {"blocks": [], "births": [], "local": {}}
                    group_order.append(owner)
                blocks[block] = event
                groups[owner]["blocks"].append(block)
            elif kind in ("birth", "mutation", "republish"):
                node, block = number(event["node"]), number(event["block"])
                need(block in blocks, "record names undeclared block")
                owner = blocks[block]["function_owner"]
                key = self.origin(event["origin"])
                number(event["legacy_id"], 16)  # Kept as diagnostic input, never used as identity.
                if kind == "birth":
                    need(node == len(births) and key not in origins, "birth IDs/origins not unique and dense")
                    if "relative" in event:
                        self.scalar(event["relative"], request, local, natural, current, event["seq"])
                    need(event["placement"] == "append" or self.args.body_only, "nonappend placement requires edit replay")
                    if event["placement"] != "append":
                        self.excluded["birth_placement:" + event["placement"]] += 1
                    restricted = groups[owner]["local"]
                    record = self.record(event, request, restricted, natural, current)
                    births.append({"event": event, "record": record})
                    groups[owner]["births"].append(node)
                    local[node], owner_of[node] = len(groups[owner]["births"]) - 1, owner
                    restricted[node] = local[node]
                    natural[node], current[node] = record[1], old_type(event["natural_type"])
                    origins.add(key)
                else:
                    need(node < len(births) and key == self.origin(births[node]["event"]["origin"]),
                         "mutation/republish lost original birth identity")
                    need(owner_of[node] == owner, "cross-owner edit cannot be normalized")
                    restricted = groups[owner]["local"]
                    self.record(event, request, restricted, natural, current)
                    if kind == "mutation":
                        mutation_count += 1
                        original = births[node]["event"]
                        metadata_only = all(event[x] == original[x] for x in ("opcode", "opcode_name", "natural_type", "type_name", "slots"))
                        need(self.args.body_only or metadata_only, "nonmetadata mutation requires edit replay")
                        if metadata_only:
                            old_record = births[node]["record"]
                            flags = event["flags"]
                            births[node]["record"] = old_record[:2] + (number(flags["mask"]), flags["profile"], int(flags["branch_only"])) + old_record[5:]
                            self.rules["metadata_mutations_folded_into_birth"] += 1
                        else:
                            self.excluded["mutation:" + event["opcode_name"]] += 1
                            excluded_seqs["nonmetadata_mutation"].append(event["seq"])
                    else:
                        need(self.args.body_only, "republish requires edit replay")
                        self.excluded["republish"] += 1
                        excluded_seqs[kind].append(event["seq"])
                    current[node] = old_type(event["natural_type"])
                last_record[node] = event
            elif kind in ("terminal", "block_frontier"):
                block = number(event["block"])
                need(block in blocks, "terminal names unknown block")
                owner = blocks[block]["function_owner"]
                restricted = groups[owner]["local"]
                self.terminal(event["terminal"], restricted, natural, current, event["seq"], request)
                terminals.append(event)
                if kind == "block_frontier":
                    need(isinstance(event["order"], list) and len(set(event["order"])) == len(event["order"]), "bad frontier roster")
                    need(all(n in restricted for n in event["order"]), "frontier contains unknown/foreign birth")
            elif kind == "request_front_finish":
                need(event["births"] == len(births) and event["mutations"] == mutation_count and
                     event["blocks"] == len(blocks) and event["missing_source_observations"] == 0,
                     "incomplete/mismatched front finish")
                finished = True
            else:
                need(self.args.body_only, "source edit requires edit replay: " + kind)
                if "node" in event:
                    need(number(event["node"]) in local, "edit names unobserved birth")
                if "block" in event:
                    need(number(event["block"]) in blocks, "edit names unknown block")
                if "relative" in event:
                    self.scalar(event["relative"], request, local, natural, current, event["seq"])
                self.excluded[kind] += 1
                excluded_seqs[kind].append(event["seq"])
        need(boolean(end["front_finished"]) == finished and end["births"] == len(births) and
             end["mutations"] == mutation_count and end["blocks"] == len(blocks), "request end counters differ")
        need(end["outcome"] in ("published", "interpreted_ir_published", "exception", "retry_as_region", "backend_null", "abandoned"), "unknown source outcome")
        publish = end["outcome"] in ("published", "interpreted_ir_published")
        need(not publish or finished, "published request lacks complete frontend observation")
        request_report = {"source_request": request, "scope": scope, "pc": begin["pc"], "begin_seq": begin["seq"],
                          "end_seq": end["seq"], "front_finished": finished, "source_outcome": end["outcome"],
                          "source_reason": end["reason"], "births": len(births), "mutations": mutation_count,
                          "source_blocks": len(blocks), "owners": len(groups), "excluded_edit_seqs": dict(excluded_seqs)}
        self.requests.append(request_report)
        for event in terminals:
            self.terminals.append({"request": request, "seq": event["seq"], "event": event["event"],
                                   "source_block": event["block"], "terminal": event["terminal"]})
        if not group_order:
            need(not publish, "published request has no actual IR owner")
            groups[None] = {"blocks": [], "births": [], "local": {}}
            group_order.append(None)  # An empty failed attempt is still charged as X.
        for owner in group_order:
            self.emit_group(request, scope, owner, groups[owner], blocks, births, terminals, publish, output)

    def emit_group(self, request, scope, owner, group, blocks, births, terminals, publish, output):
        source_blocks = group["blocks"]
        block_ids = {b: i for i, b in enumerate(source_blocks)}
        segments, used, replay_blocks = [], set(), len(source_blocks)
        for node in group["births"]:
            block = births[node]["event"]["block"]
            if not segments or segments[-1]["source_block"] != block:
                if block in used:
                    need(self.args.body_only, "FULL_TERMINALS cannot project a source block revisit")
                    replay_id = replay_blocks
                    replay_blocks += 1
                else:
                    replay_id = block_ids[block]
                segments.append({"source_block": block, "replay_block": replay_id, "source_node_ids": []})
                used.add(block)
            segments[-1]["source_node_ids"].append(node)
        segments += [{"source_block": b, "replay_block": block_ids[b], "source_node_ids": []}
                     for b in source_blocks if b not in used]
        qid = len(self.queries)
        output.write("Q %d %d %d %d %d\n" % (qid, int(scope == "function"),
                    {"known": 0, "early_off": 1, "unknown": 2}[self.args.mode], self.args.target, self.args.features))
        for block_id in range(replay_blocks):
            output.write("C %d\n" % block_id)
        cursor, uses, words, cells = 0, 0, 0, 0
        for segment in segments:
            block = segment["source_block"]
            output.write("B %d\n" % segment["replay_block"])
            for node in segment["source_node_ids"]:
                need(node == group["births"][cursor], "projection changes source birth order")
                name, result, mask, profile, branch, operands = births[node]["record"]
                need(len(operands) <= 1000000, "instruction exceeds replay atom limit")
                output.write("I %d %s %s %d %d %d %d\n" % (cursor, name, result, mask, profile, branch, len(operands)))
                for cell in operands:
                    output.write("V %d %s %d %d %d\n" % cell)
                    uses += cell[0] == 1
                    words += 2 if cell[0] in (2, 6, 9) and cell[2] > INVALID else 1
                cells += len(operands)
                cursor += 1
            if self.args.body_only:
                terminal = (4, 0, INVALID, "Void", INVALID, INVALID)
            else:
                actual = [t for t in terminals if t["block"] == block and t["event"] == "block_frontier"]
                need(len(actual) == 1, "missing unique final terminal")
                raw = actual[0]["terminal"]
                if raw["kind"] == "ReturnToDispatch":
                    terminal = (5, 0, INVALID, "Void", INVALID, INVALID)
                elif raw["kind"] == "ReturnToHost":
                    terminal = (1, 0, INVALID, "Void", INVALID, INVALID)
                elif raw["kind"] == "LinkBlock":
                    targets = [b for b in source_blocks if number(blocks[b]["location"]) == number(raw["next"])]
                    need(len(targets) == 1, "LinkBlock is not a unique owner-local target")
                    terminal = (2, 0, INVALID, "Void", block_ids[targets[0]], INVALID)
                else:
                    raise ValueError("FULL_TERMINALS cannot represent source " + raw["kind"])
            output.write("T %d %d %d %s %d %d\n" % terminal)
        need(cursor == len(group["births"]), "unemitted births")
        output.write("P\n" if publish else "X\n")
        self.queries.append({"id": qid, "source_request": request, "scope": scope, "function_owner": owner,
                             "source_node_ids": group["births"], "source_block_ids": source_blocks,
                             "source_blocks": len(source_blocks), "replay_blocks": replay_blocks,
                             "extra_segments": replay_blocks - len(source_blocks), "segments": segments,
                             "N": cursor, "E": uses,
                             "logical_cells": cells, "physical_words": words, "outcome": "P" if publish else "X"})
        self.nodes += cursor
        self.uses += uses
        self.words += words
        self.atoms += cells
        self.emitted_events += replay_blocks * 3 + cursor

    def run(self, output):
        active, seen, seq = {}, set(), 0
        output.write("SVMIR_REPLAY_1 " + ("BODY_ONLY" if self.args.body_only else "FULL_TERMINALS") + "\n")
        with self.args.raw.open() as stream:
            for line in stream:
                self.context = "raw line %d" % (seq + 1)
                event = json.loads(line)
                need(isinstance(event, dict) and type(event.get("seq")) is int and event["seq"] == seq and self.footer is None, "missing/damaged sequence or trailing data")
                kind = event.get("event")
                self.events[kind] += 1
                seq += 1
                if kind == "header":
                    fields(event, {"seq", "event", "version", "cohort", "source_digest", "pid", "limit_bytes", "max_physical_slots", "missing_slots_are_Void"})
                    need(self.header is None and seq == 1 and event["version"] == 1 and event["source_digest"] == COHORT and
                         event["max_physical_slots"] == 5 and event["missing_slots_are_Void"] is True, "unrecognized raw capture contract")
                    self.header = event
                elif kind == "footer":
                    fields(event, {"seq", "event", "requests", "ended", "complete"})
                    need(not active and event["complete"] is True and event["requests"] == event["ended"] == len(seen), "incomplete footer")
                    self.footer = event
                else:
                    need(self.header is not None and kind in EVENT_FIELDS, "unknown/missing raw event contract")
                    fields(event, COMMON | EVENT_FIELDS[kind], {"relative"} if kind == "birth" else ())
                    request = number(event["request"])
                    if kind == "request_begin":
                        need(request not in seen, "repeated source request ID")
                        seen.add(request)
                        active[request] = []
                    need(request in active, "event outside source request")
                    active[request].append(event)
                    if kind == "request_end":
                        self.write_request(active.pop(request), output)
        need(self.header is not None and self.footer is not None and not active, "truncated raw capture")
        need(len(self.queries) <= 100000 and self.emitted_events <= 2000000 and self.atoms <= 8000000,
             "normalized input exceeds replay reader limits")

    def report(self):
        receipt = json.loads(self.args.capture_receipt.read_text())
        devices = set()
        for command in receipt.get("commands", []):
            argv = command.get("argv", [])
            if "-s" in argv:
                devices.add(argv[argv.index("-s") + 1])
        need(len(devices) == 1 and receipt.get("closure") == "closed", "capture receipt missing unique device/closed lifetime")
        return {"schema": "birth-normalization-1", "status": "normalized_all_requests",
                "measurement_kind": "birth_ir_writer_replay", "application_compile_eligible": False,
                "body_only": self.args.body_only, "capture_run_mode": self.args.capture_run_mode,
                "raw": digest(self.args.raw), "capture_receipt": digest(self.args.capture_receipt),
                "capture_device": next(iter(devices)), "header": self.header, "footer": self.footer,
                "normalizer": digest(Path(__file__)),
                "schemas": [digest(self.args.old_source / "source/runtime/ir/ir.inc"), digest(self.args.new_source / "source/runtime/ir/opcodes.inc")],
                "target_metadata": {"source": "explicit_not_observed", "mode": self.args.mode, "target": self.args.target, "features": self.args.features},
                "counts": {"source_requests": len(self.requests), "replay_requests": len(self.queries),
                           "actual_source_scopes": dict(Counter(r["scope"] for r in self.requests)),
                           "block_fallback_requests": sum(r["scope"] == "block" for r in self.requests) if self.args.capture_run_mode == "function" else 0,
                           "source_outcomes": dict(Counter(r["source_outcome"] for r in self.requests)),
                           "replay_outcomes": dict(Counter(q["outcome"] for q in self.queries)),
                           "source_blocks": sum(q["source_blocks"] for q in self.queries),
                           "replay_blocks": sum(q["replay_blocks"] for q in self.queries),
                           "extra_segments": sum(q["extra_segments"] for q in self.queries),
                           "N": self.nodes, "E_body_occurrences": self.uses, "logical_cells": self.atoms, "physical_words": self.words,
                           "raw_events": dict(self.events), "normalization_rules": dict(self.rules),
                           "excluded_edit_events": dict(self.excluded), "source_terminal_records": len(self.terminals),
                           "replacement_terminal_records": sum(q["replay_blocks"] for q in self.queries) if self.args.body_only else 0},
                "requests": self.requests, "replay_requests": self.queries, "source_terminals": self.terminals,
                "source_uniform_effect_dictionary": {key: {"effect_set": value, "raw_uses": self.effects_uses[key]} for key, value in self.effects.items()},
                "limitations": ["All birth payloads are retained in source birth order; BODY_ONLY excludes subsequent edits and final program semantics.",
                                "Target capabilities are explicit replay metadata, not observed capture capabilities.",
                                "Original Flags ABI bits are preserved; old BOOL natural U8 becomes Bool with explicit old read views where necessary.",
                                "Lambda traits retain all eight u8 values. Source UniformEffectSet dictionaries are external sidecar evidence, not a production registry or executable helper contract.",
                                "Block declarations may be frontloaded within each owner; raw event order remains the authority.",
                                "BODY_ONLY projects revisits into extra sequential blocks; their real Create/Begin/End costs are counted. This does not preserve original Function layout, CFG, or RA input.",
                                "Writer replay is neither decode nor application compile and cannot pass the A/full budget gate."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--capture-receipt", type=Path, required=True)
    parser.add_argument("--capture-run-mode", choices=("block", "function"), required=True)
    parser.add_argument("--mode", choices=("known", "early_off", "unknown"), required=True)
    parser.add_argument("--target", type=lambda s: number(s, 32), required=True)
    parser.add_argument("--features", type=number, required=True)
    parser.add_argument("--body-only", action="store_true")
    parser.add_argument("--old-source", type=Path, default=Path("/Volumes/Data2/SwiftVM"))
    parser.add_argument("--new-source", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    normalizer, temporary = None, None
    try:
        need(args.raw.resolve() not in (args.output.resolve(), args.report.resolve()) and args.output.resolve() != args.report.resolve(), "input/output paths overlap")
        need(not args.output.exists() and not args.report.exists(), "refusing to overwrite existing evidence")
        normalizer = Normalizer(args)
        with tempfile.NamedTemporaryFile("w", dir=args.output.parent, prefix=args.output.name + ".", delete=False) as stream:
            temporary = Path(stream.name)
            normalizer.run(stream)
        report = normalizer.report()
        report["output"] = digest(temporary) | {"path": str(args.output.resolve())}
        args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        temporary.replace(args.output)
        temporary = None
        print(json.dumps({"status": report["status"], "output": str(args.output), "counts": report["counts"]}, allow_nan=False))
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(json.dumps({"status": "FAILED_CLOSED", "context": normalizer.context if normalizer else "schema/arguments", "error": str(error)}), file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
