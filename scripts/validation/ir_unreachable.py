"""Conservatively certify impossible LLVM switch defaults, without executing IR.

This is a structural check for separately verified LLVM IR, not an IR verifier
or a proof of instruction/native semantics. A block containing ``unreachable``
is accepted only if it consists of that terminator, is not the function entry,
and EVERY incoming CFG edge is a switch default whose possible selector values
are covered by its explicit cases. No path, image, input-sample or ABI facts are
used. Unsupported syntax or exhausted limits retains the violation.

The selector domain is a known-bits over-approximation. Ordinary integer loads
and integer arguments denote arbitrary defined bitvectors; load addresses are
checked through a small plain-pointer DAG, without assuming memory contents.
PHIs, undef/poison, freeze, calls and unproved arithmetic flags are rejected.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re

_NAME = r"[-A-Za-z$._0-9]+"
_LOCAL = r"%" + _NAME
_PTR = r"(?:%|@)" + _NAME
_ATTACHMENTS = r"(?:\s*,\s*![A-Za-z0-9_.$-]+\s+![0-9]+)*"
# Debug locations carry no value precondition. Other load metadata may assert
# range/nonnull/definedness or alias properties; do not silently assume it.
_LOAD_ATTACHMENTS = r"(?:\s*,\s*!dbg\s+![0-9]+)*"


@dataclass(frozen=True)
class Limits:
    max_ir_bytes: int = 16 * 1024 * 1024
    max_lines: int = 200000
    max_work: int = 1000000
    max_nodes: int = 256
    max_depth: int = 64
    max_values: int = 256
    max_blocks: int = 16384
    max_edges: int = 131072


class _Reject(Exception):
    pass


class _Budget:
    def __init__(self, limits):
        self.limits = limits
        self.work = 0

    def take(self, amount=1):
        if self.work + amount > self.limits.max_work:
            raise _Reject("proof work budget exhausted")
        self.work += amount


def _code(line):
    """Strip a comment without treating a semicolon in a quoted name as one."""
    quoted = escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
        elif char == "\\" and quoted:
            escaped = True
        elif char == '"':
            quoted = not quoted
        elif char == ";" and not quoted:
            return line[:index].strip()
    return line.strip()


def _split(text):
    result = []
    start = depth = 0
    quoted = escaped = False
    for index, char in enumerate(text):
        if escaped:
            escaped = False
        elif char == "\\" and quoted:
            escaped = True
        elif char == '"':
            quoted = not quoted
        elif not quoted:
            if char in "([{<":
                depth += 1
            elif char in ")]}>" :
                depth -= 1
            elif char == "," and depth == 0:
                result.append(text[start:index].strip())
                start = index + 1
    if depth or quoted:
        raise _Reject("unsupported unbalanced operand syntax")
    result.append(text[start:].strip())
    return result


def _width(value):
    width = int(value)
    if not 1 <= width <= 64:
        raise _Reject("integer width outside 1..64")
    return width


def _constant(token, width):
    if token in ("undef", "poison"):
        raise _Reject("undef/poison selector dependency")
    if width == 1 and token in ("true", "false"):
        return int(token == "true")
    if not re.fullmatch(r"-?[0-9]+", token):
        raise _Reject("unsupported integer operand " + token)
    value = int(token)
    if not -(1 << (width - 1)) <= value < (1 << width):
        raise _Reject("integer literal does not fit declared width")
    return value & ((1 << width) - 1)


def _parameters(header, function_name):
    start = header.index("(", header.index("@" + function_name))
    depth = 1
    end = start + 1
    while end < len(header) and depth:
        if header[end] == "(": depth += 1
        elif header[end] == ")": depth -= 1
        end += 1
    if depth:
        raise _Reject("unsupported function parameter syntax")
    integers = {}
    pointers = set()
    for parameter in _split(header[start + 1:end - 1]):
        name = re.search(r"(" + _LOCAL + r")\s*$", parameter)
        if not name:
            continue
        typed = re.match(r"i([0-9]+)\b", parameter)
        if typed:
            integers[name[1]] = int(typed[1])
        elif re.match(r"ptr\b", parameter):
            pointers.add(name[1])
    return integers, pointers


def _parse(ir, limits, budget):
    functions = []
    current = None
    block = None
    statement = []
    start_line = 0
    lines = ir.splitlines()
    if len(lines) > limits.max_lines:
        raise _Reject("IR line budget exhausted")
    for number, raw in enumerate(lines, 1):
        budget.take()
        text = _code(raw)
        if not text:
            continue
        if current is None:
            if text.startswith("define "):
                name = re.search(r"@(" + _NAME + r")\(", text)
                if not name or not text.endswith("{"):
                    raise _Reject("unsupported function header")
                integers, pointers = _parameters(text, name[1])
                current = dict(name=name[1], blocks={}, definitions={}, integers=integers,
                               pointers=pointers, first=None)
                functions.append(current)
            elif re.match(r"unreachable\b", text):
                raise _Reject("unreachable outside a parsed function")
            continue
        if statement:
            statement.append(text)
            if "]" not in text:
                continue
            text = " ".join(statement)
            statement = []
            number_for_statement = start_line
        else:
            number_for_statement = number
        if text == "}":
            if not current["blocks"]:
                raise _Reject("empty parsed function")
            current = block = None
            continue
        label = re.fullmatch(r"(" + _NAME + r"):", text)
        if label:
            if label[1] in current["blocks"]:
                raise _Reject("duplicate block label")
            block = dict(name=label[1], statements=[])
            current["blocks"][label[1]] = block
            if current["first"] is None: current["first"] = label[1]
            if len(current["blocks"]) > limits.max_blocks:
                raise _Reject("CFG block budget exhausted")
            continue
        if block is None:
            block = dict(name="<implicit-entry>", statements=[])
            current["blocks"][block["name"]] = block
            current["first"] = block["name"]
        if text.startswith("switch ") and "]" not in text:
            statement = [text]
            start_line = number
            continue
        block["statements"].append((number_for_statement, text))
        definition = re.match(r"(" + _LOCAL + r")\s*=\s*(.*)$", text)
        if definition:
            name, rhs = definition.groups()
            if name in current["definitions"] or name in current["integers"] or name in current["pointers"]:
                raise _Reject("duplicate SSA definition")
            current["definitions"][name] = (number_for_statement, rhs)
    if current is not None or statement:
        raise _Reject("unterminated function or switch")
    return functions


def _terminator(statement):
    if re.fullmatch(r"unreachable" + _ATTACHMENTS, statement):
        return dict(kind="unreachable", edges=[])
    if statement.startswith("ret "):
        return dict(kind="return", edges=[])
    direct = re.fullmatch(r"br\s+label\s+%(" + _NAME + r")" + _ATTACHMENTS, statement)
    if direct:
        return dict(kind="branch", edges=[(direct[1], "branch")])
    conditional = re.fullmatch(r"br\s+i1\s+[^,]+,\s*label\s+%(" + _NAME +
                               r"),\s*label\s+%(" + _NAME + r")" + _ATTACHMENTS, statement)
    if conditional:
        return dict(kind="conditional", edges=[(conditional[1], "conditional"), (conditional[2], "conditional")])
    switch = re.fullmatch(r"switch\s+i([0-9]+)\s+([^,]+),\s*label\s+%(" + _NAME +
                          r")\s*\[(.*)\]" + _ATTACHMENTS, statement)
    if switch:
        width = _width(switch[1]); body = switch[4].strip(); cases = []
        pattern = re.compile(r"i([0-9]+)\s+(-?[0-9]+|true|false),\s*label\s+%(" + _NAME + r")")
        previous = 0
        for item in pattern.finditer(body):
            if body[previous:item.start()].strip(): raise _Reject("unsupported switch case syntax")
            if int(item[1]) != width: raise _Reject("switch case width mismatch")
            cases.append((_constant(item[2], width), item[3])); previous = item.end()
        if body[previous:].strip() or len({value for value, _ in cases}) != len(cases):
            raise _Reject("unsupported or duplicate switch case")
        return dict(kind="switch", width=width, selector=switch[2].strip(), cases=cases,
                    default=switch[3], edges=[(switch[3], "default")] + [(target, "case") for _, target in cases])
    raise _Reject("unsupported CFG terminator")


@dataclass(frozen=True)
class _Bits:
    width: int
    zero: int
    one: int

    @property
    def mask(self): return (1 << self.width) - 1


class _KnownBits:
    def __init__(self, function, budget):
        self.function = function
        self.budget = budget
        self.cache = {}
        self.pointer_cache = set()
        self.pointer_nodes = []
        self.active = set()
        self.nodes = []
        self.seen = set()

    def _begin(self, key, depth):
        self.budget.take()
        if depth > self.budget.limits.max_depth: raise _Reject("selector depth budget exhausted")
        if key in self.active: raise _Reject("cyclic selector dependency")
        if key not in self.seen:
            self.seen.add(key)
            if len(self.seen) > self.budget.limits.max_nodes: raise _Reject("selector node budget exhausted")
        self.active.add(key)

    def pointer(self, token, depth):
        if token in self.pointer_cache: return
        key = ("pointer", token); self._begin(key, depth)
        try:
            line = rhs = None
            if token in self.function["pointers"] or re.fullmatch(r"@" + _NAME, token):
                kind = "arbitrary-defined-pointer"
            else:
                definition = self.function["definitions"].get(token)
                if definition is None: raise _Reject("unknown pointer dependency")
                line, rhs = definition
                if re.search(r"\b(?:undef|poison)\b", rhs): raise _Reject("undef/poison pointer dependency")
                load = re.fullmatch(r"load\s+ptr,\s*ptr\s+(" + _PTR + r")(?:,\s*align\s+[0-9]+)?" + _LOAD_ATTACHMENTS, rhs)
                gep = re.fullmatch(r"getelementptr\s+i8,\s*ptr\s+(" + _PTR + r")((?:,\s*i[0-9]+\s+-?[0-9]+)+)", rhs)
                if load:
                    self.pointer(load[1], depth + 1)
                    kind = "plain-pointer-load-no-content-assumption"
                elif gep:
                    self.pointer(gep[1], depth + 1)
                    for index in _split(gep[2].lstrip(",")):
                        typed = re.fullmatch(r"i([0-9]+)\s+(-?[0-9]+)", index)
                        if typed is None: raise _Reject("unsupported pointer index")
                        self.budget.take(); _constant(typed[2], _width(typed[1]))
                    kind = "plain-i8-GEP-with-integer-indices"
                else: raise _Reject("unsupported pointer dependency or flags")
            self.pointer_cache.add(token)
            self.pointer_nodes.append(dict(value=token, line=line, instruction=rhs, kind=kind,
                                           memory_contents_assumed=False))
        finally:
            self.active.remove(key)

    def value(self, token, width, depth=0):
        width = _width(width); key = (token, width)
        if key in self.cache: return self.cache[key]
        self._begin(key, depth)
        try:
            mask = (1 << width) - 1; line = None; rhs = None; checks = []
            if not token.startswith("%"):
                number = _constant(token, width); result = _Bits(width, mask ^ number, number)
                kind = "integer-constant"
            elif token in self.function["integers"]:
                if self.function["integers"][token] != width: raise _Reject("argument width mismatch")
                result = _Bits(width, 0, 0); kind = "arbitrary-defined-integer-argument"
            else:
                definition = self.function["definitions"].get(token)
                if definition is None: raise _Reject("unknown integer SSA dependency")
                line, rhs = definition
                if re.search(r"\b(?:undef|poison)\b", rhs): raise _Reject("undef/poison selector dependency")
                binary = re.fullmatch(r"(and|or|xor)\s+(?:(disjoint)\s+)?i([0-9]+)\s+([^,]+),\s*(.+)", rhs)
                cast = re.fullmatch(r"(trunc|zext)\s+i([0-9]+)\s+(.+)\s+to\s+i([0-9]+)", rhs)
                load = re.fullmatch(r"load\s+i([0-9]+),\s*ptr\s+(" + _PTR + r")(?:,\s*align\s+[0-9]+)?" + _LOAD_ATTACHMENTS, rhs)
                if binary:
                    operation, flag, bits, left, right = binary.groups()
                    if int(bits) != width: raise _Reject("binary width mismatch")
                    if flag and operation != "or": raise _Reject("unsupported arithmetic flag")
                    a, b = self.value(left.strip(), width, depth + 1), self.value(right.strip(), width, depth + 1)
                    if flag:
                        intersection = (mask ^ a.zero) & (mask ^ b.zero)
                        if intersection: raise _Reject("or disjoint precondition unproved")
                        checks.append(dict(kind="disjoint-proved", possible_one_intersection=0))
                    if operation == "and": result = _Bits(width, a.zero | b.zero, a.one & b.one)
                    elif operation == "or": result = _Bits(width, a.zero & b.zero, a.one | b.one)
                    else: result = _Bits(width, (a.zero & b.zero) | (a.one & b.one), (a.zero & b.one) | (a.one & b.zero))
                    kind = operation
                elif cast:
                    operation, source_width, operand, dest_width = cast.groups()
                    source_width = _width(source_width)
                    if int(dest_width) != width or (operation == "trunc" and source_width <= width) or (operation == "zext" and source_width >= width):
                        raise _Reject("invalid cast widths")
                    source = self.value(operand.strip(), source_width, depth + 1)
                    result = _Bits(width, (source.zero | (mask ^ source.mask)) & mask if operation == "zext" else source.zero & mask, source.one & mask)
                    kind = operation
                elif load:
                    if int(load[1]) != width: raise _Reject("load width mismatch")
                    self.pointer(load[2], depth + 1)
                    result = _Bits(width, 0, 0); kind = "arbitrary-defined-integer-load"
                else:
                    raise _Reject("unsupported integer dependency or dangerous flags")
            if result.zero & result.one or (result.zero | result.one) & ~mask:
                raise _Reject("invalid known-bits invariant")
            self.nodes.append(dict(value=token, width=width, kind=kind, line=line, instruction=rhs,
                                   known_zero=hex(result.zero), known_one=hex(result.one), checks=checks))
            self.cache[key] = result
            return result
        finally:
            self.active.remove(key)

    def possible_values(self, token, width):
        bits = self.value(token, width); variable = bits.mask ^ (bits.zero | bits.one)
        count = variable.bit_count()
        if (1 << count) > self.budget.limits.max_values:
            raise _Reject("possible selector domain exceeds value budget")
        values = []; subset = variable
        while True:
            self.budget.take(); values.append(bits.one | subset)
            if subset == 0: break
            subset = (subset - 1) & variable
        return sorted(values), bits


def audit_unreachable(ir: str, *, limits: Limits | None = None) -> dict:
    """Return a JSON-compatible certificate; ``ok`` only clears this IR check."""
    limits = limits or Limits(); budget = _Budget(limits)
    report = dict(schema=1, checker="dead-switch-default-known-bits-v1", ok=False,
                  ir_sha256=None, checker_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), limits=asdict(limits),
                  unreachable_count=0, proved_dead_count=0, certificates=[], violations=[],
                  scope="Separately verified LLVM IR; no path/image/ABI/sample assumptions. Known-bits enumeration is a full over-approximation, not correlated input sampling.")
    try:
        hard = asdict(Limits())
        if any(type(value) is not int or not 0 < value <= hard[name] for name, value in asdict(limits).items()):
            raise _Reject("invalid proof limits")
        # Bound work and allocation before hashing/splitting an oversized input.
        if len(ir) > limits.max_ir_bytes: raise _Reject("IR byte budget exhausted")
        if ir.count("\n") + 1 > limits.max_lines: raise _Reject("IR line budget exhausted")
        ir_hash = hashlib.sha256(); byte_count = 0
        for offset in range(0, len(ir), 4096):
            encoded_piece = ir[offset:offset + 4096].encode("utf-8")
            byte_count += len(encoded_piece)
            if byte_count > limits.max_ir_bytes: raise _Reject("IR byte budget exhausted")
            ir_hash.update(encoded_piece)
        report["ir_sha256"] = ir_hash.hexdigest()
        sites = [(number, _code(line)) for number, line in enumerate(ir.splitlines(), 1)
                 if re.match(r"unreachable(?:\b|,)", _code(line))]
        report["unreachable_count"] = len(sites)
        if not sites:
            report["ok"] = True
            return report | dict(work=budget.work)
        functions = _parse(ir, limits, budget)
        for function in functions:
            blocks = function["blocks"]; targets = []
            for block in blocks.values():
                targets.extend((block, line) for line, text in block["statements"] if re.match(r"unreachable(?:\b|,)", text))
            if not targets: continue
            try:
                incoming = {name: [] for name in blocks}; terminators = {}; edges = 0
                for name, block in blocks.items():
                    if not block["statements"]: raise _Reject("empty CFG block")
                    terminator = _terminator(block["statements"][-1][1]); terminators[name] = terminator
                    for destination, kind in terminator["edges"]:
                        budget.take(); edges += 1
                        if edges > limits.max_edges: raise _Reject("CFG edge budget exhausted")
                        if destination not in blocks: raise _Reject("CFG target is not a parsed block")
                        incoming[destination].append((name, kind))
                evaluator = _KnownBits(function, budget)
                for block, line in targets:
                    try:
                        if block["name"] == function["first"]: raise _Reject("unreachable function entry")
                        if len(block["statements"]) != 1 or terminators[block["name"]]["kind"] != "unreachable":
                            raise _Reject("unreachable block has other instructions")
                        predecessors = incoming[block["name"]]
                        if not predecessors: raise _Reject("unreachable block has no switch-default predecessor")
                        conditions = []
                        for source, edge_kind in predecessors:
                            if edge_kind != "default": raise _Reject("non-default incoming edge")
                            switch = terminators[source]
                            if switch["kind"] != "switch": raise _Reject("incoming source is not a switch")
                            values, known = evaluator.possible_values(switch["selector"], switch["width"])
                            cases = sorted(value for value, _ in switch["cases"])
                            missing = sorted(set(values) - set(cases))
                            if missing: raise _Reject("switch default may be selected; explicit cases miss possible values")
                            conditions.append(dict(predecessor=source, incoming_edge="switch-default", selector=switch["selector"],
                                width=switch["width"], known_zero=hex(known.zero), known_one=hex(known.one),
                                possible_values_overapprox=values, case_values=cases, missing_values=missing,
                                exhaustive_known_bits_cube=True, every_possible_value_is_an_explicit_case=True))
                        report["certificates"].append(dict(function=function["name"], block=block["name"], line=line,
                            all_incoming_edges_checked=True, predecessor_count=len(predecessors), conditions=conditions,
                            selector_dependencies=list(evaluator.nodes), pointer_dependencies=list(evaluator.pointer_nodes),
                            result="proved-dead-switch-default"))
                        report["proved_dead_count"] += 1
                    except _Reject as error:
                        report["violations"].append(dict(function=function["name"], block=block["name"], line=line, reason=str(error)))
            except _Reject as error:
                report["violations"].extend(dict(function=function["name"], block=block["name"], line=line, reason=str(error)) for block, line in targets)
        if report["proved_dead_count"] + len(report["violations"]) != len(sites):
            raise _Reject("not every unreachable instruction was accounted for")
        report["ok"] = report["proved_dead_count"] == len(sites) and not report["violations"]
    except (ValueError, KeyError, TypeError, RecursionError, _Reject) as error:
        report["ok"] = False
        report["violations"].append(dict(reason=str(error), scope="module-or-proof-budget"))
    return report | dict(work=budget.work)


def has_unproved_unreachable(ir: str, *, limits: Limits | None = None) -> bool:
    return not audit_unreachable(ir, limits=limits)["ok"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path); parser.add_argument("--certificate", type=Path)
    args = parser.parse_args(); report = audit_unreachable(args.input.read_text())
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.certificate:
        with args.certificate.open("x") as stream: stream.write(text)
    else: print(text, end="")
    return 0 if report["ok"] else 1


if __name__ == "__main__": raise SystemExit(main())
