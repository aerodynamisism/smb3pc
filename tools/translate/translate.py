#!/usr/bin/env python3
"""Static 6502 -> C++ translator for the captainsouthbird/smb3 disassembly.

The translator reads the NESASM source (the authoritative code/data split),
tracks every address, encodes every instruction and data directive, and
compares the result byte-for-byte with the user's ROM. Code is only emitted
when that verification passes with zero mismatches.

Generated code layout: one C++ function per PRG bank (8 KB file bank).
Each function is a `switch` over every instruction address in that bank
followed by the instructions themselves as labelled statements, so control
flow inside a bank uses real gotos. Anything that leaves the bank (JSR, RTS,
cross-bank JMP/branch, RTI, computed jumps) returns the next 16-bit CPU address
to the runtime dispatcher, which maps it to the bank currently mapped there.
The 6502 stack lives in RAM, so RTS-based dispatch tables work unchanged.

Usage:
  translate.py --src tools/translate/smb3-src/smb3.asm --rom ROM.nes --out build/gen
"""
import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# 6502 opcode table: mnemonic -> mode -> (opcode, size, base cycles)
# ---------------------------------------------------------------------------
M = {}


def _op(mn, mode, opc, size, cyc):
    M.setdefault(mn, {})[mode] = (opc, size, cyc)


_alu = {  # mnemonic: (imm, zp, zpx, abs, absx, absy, indx, indy)
    "ADC": (0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71),
    "AND": (0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31),
    "CMP": (0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1),
    "EOR": (0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51),
    "LDA": (0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1),
    "ORA": (0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11),
    "SBC": (0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1),
}
for _mn, (_i, _z, _zx, _a, _ax, _ay, _ix, _iy) in _alu.items():
    _op(_mn, "imm", _i, 2, 2)
    _op(_mn, "zp", _z, 2, 3)
    _op(_mn, "zpx", _zx, 2, 4)
    _op(_mn, "abs", _a, 3, 4)
    _op(_mn, "absx", _ax, 3, 4)
    _op(_mn, "absy", _ay, 3, 4)
    _op(_mn, "indx", _ix, 2, 6)
    _op(_mn, "indy", _iy, 2, 5)

# read-modify-write group: mnemonic -> (zp, zpx, abs, absx, acc?)
_rmw = {
    "ASL": (0x06, 0x16, 0x0E, 0x1E, 0x0A),
    "LSR": (0x46, 0x56, 0x4E, 0x5E, 0x4A),
    "ROL": (0x26, 0x36, 0x2E, 0x3E, 0x2A),
    "ROR": (0x66, 0x76, 0x6E, 0x7E, 0x6A),
    "DEC": (0xC6, 0xD6, 0xCE, 0xDE, None),
    "INC": (0xE6, 0xF6, 0xEE, 0xFE, None),
}
for _mn, (_z, _zx, _a, _ax, _acc) in _rmw.items():
    _op(_mn, "zp", _z, 2, 5)
    _op(_mn, "zpx", _zx, 2, 6)
    _op(_mn, "abs", _a, 3, 6)
    _op(_mn, "absx", _ax, 3, 7)
    if _acc is not None:
        _op(_mn, "acc", _acc, 1, 2)

_op("LDX", "imm", 0xA2, 2, 2)
_op("LDX", "zp", 0xA6, 2, 3)
_op("LDX", "zpy", 0xB6, 2, 4)
_op("LDX", "abs", 0xAE, 3, 4)
_op("LDX", "absy", 0xBE, 3, 4)
_op("LDY", "imm", 0xA0, 2, 2)
_op("LDY", "zp", 0xA4, 2, 3)
_op("LDY", "zpx", 0xB4, 2, 4)
_op("LDY", "abs", 0xAC, 3, 4)
_op("LDY", "absx", 0xBC, 3, 4)
_op("STA", "zp", 0x85, 2, 3)
_op("STA", "zpx", 0x95, 2, 4)
_op("STA", "abs", 0x8D, 3, 4)
_op("STA", "absx", 0x9D, 3, 5)
_op("STA", "absy", 0x99, 3, 5)
_op("STA", "indx", 0x81, 2, 6)
_op("STA", "indy", 0x91, 2, 6)
_op("STX", "zp", 0x86, 2, 3)
_op("STX", "zpy", 0x96, 2, 4)
_op("STX", "abs", 0x8E, 3, 4)
_op("STY", "zp", 0x84, 2, 3)
_op("STY", "zpx", 0x94, 2, 4)
_op("STY", "abs", 0x8C, 3, 4)
_op("CPX", "imm", 0xE0, 2, 2)
_op("CPX", "zp", 0xE4, 2, 3)
_op("CPX", "abs", 0xEC, 3, 4)
_op("CPY", "imm", 0xC0, 2, 2)
_op("CPY", "zp", 0xC4, 2, 3)
_op("CPY", "abs", 0xCC, 3, 4)
_op("BIT", "zp", 0x24, 2, 3)
_op("BIT", "abs", 0x2C, 3, 4)
_op("JMP", "abs", 0x4C, 3, 3)
_op("JMP", "ind", 0x6C, 3, 5)
_op("JSR", "abs", 0x20, 3, 6)
_op("RTS", "impl", 0x60, 1, 6)
_op("RTI", "impl", 0x40, 1, 6)
_op("BRK", "impl", 0x00, 1, 7)
for _mn, _opc in [("BPL", 0x10), ("BMI", 0x30), ("BVC", 0x50), ("BVS", 0x70),
                  ("BCC", 0x90), ("BCS", 0xB0), ("BNE", 0xD0), ("BEQ", 0xF0)]:
    _op(_mn, "rel", _opc, 2, 2)
for _mn, _opc in [("CLC", 0x18), ("SEC", 0x38), ("CLI", 0x58), ("SEI", 0x78),
                  ("CLV", 0xB8), ("CLD", 0xD8), ("SED", 0xF8), ("TAX", 0xAA),
                  ("TXA", 0x8A), ("TAY", 0xA8), ("TYA", 0x98), ("TSX", 0xBA),
                  ("TXS", 0x9A), ("DEX", 0xCA), ("DEY", 0x88), ("INX", 0xE8),
                  ("INY", 0xC8), ("NOP", 0xEA), ("PHA", 0x48), ("PHP", 0x08),
                  ("PLA", 0x68), ("PLP", 0x28)]:
    _cyc = {"PHA": 3, "PHP": 3, "PLA": 4, "PLP": 4}.get(_mn, 2)
    _op(_mn, "impl", _opc, 1, _cyc)

# Pseudo-ops / macros from the disassembly header: name -> expansion template
# Each entry: (number of operand-bearing expansions, handler key)
MACRO_BRANCH = {"BLT": "BCC", "BGE": "BCS", "BLS": "BMI", "BGS": "BPL"}
MACRO_ALU = {"ADD": ("CLC", "ADC"), "SUB": ("SEC", "SBC")}
IGNORED_MACROS = {"BoundCheck", "DMCAlign"}

# NESASM function-style defines: NAME .func (expr using \1), called as NAME(arg).
FUNCS = {}  # name -> (arg count, body with \N replaced by placeholders)
LABEL_BANKS = {}  # label name -> ROM bank it was laid out in (for BANK(label))
FUNC_ARG = "__funcarg"
FUNC_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*):?\s+\.func\s+(.+)$")

# Zero-page-capable modes for mnemonics whose operand may be forced/short
ZP_FALLBACK = {"zp": "abs", "zpx": "absx", "zpy": "absy"}

# ---------------------------------------------------------------------------
# Expression evaluation (NESASM-style: $hex, %bin, decimal, 'c', <lo, >hi)
# ---------------------------------------------------------------------------
TOKEN_RE = re.compile(r"""
    \s*(
      \$[0-9A-Fa-f]+ | %[01]+ | 0x[0-9A-Fa-f]+ | [0-9]+ |
      '.' |
      [A-Za-z_.][A-Za-z0-9_.]* |
      <<|>>|[-+*/%&|^()<>~!,]
    )""", re.VERBOSE)


class Unknown(Exception):
    pass


def tokenize(s):
    pos = 0
    out = []
    s = s.rstrip()
    while pos < len(s):
        m = TOKEN_RE.match(s, pos)
        if not m or m.end() == pos:
            if s[pos:].strip() == "":
                break
            raise ValueError("bad expression near %r in %r" % (s[pos:], s))
        out.append(m.group(1))
        pos = m.end()
    return out


class ExprParser:
    BIN = {"|": 1, "^": 2, "&": 3, "<<": 4, ">>": 4, "+": 5, "-": 5,
           "*": 6, "/": 6, "%": 6}

    def __init__(self, text, syms):
        self.toks = tokenize(text)
        self.i = 0
        self.syms = syms

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self):
        t = self.peek()
        self.i += 1
        return t

    def parse(self):
        v = self.expr(0)
        if self.i != len(self.toks):
            raise ValueError("trailing tokens in expression: %r" % self.toks[self.i:])
        return v

    def expr(self, minp):
        lhs = self.unary()
        while True:
            t = self.peek()
            if t not in self.BIN or self.BIN[t] < minp or self.BIN[t] == 0:
                break
            p = self.BIN[t]
            self.take()
            rhs = self.expr(p + 1)
            lhs = self.apply(t, lhs, rhs)
        return lhs

    @staticmethod
    def apply(op, a, b):
        if op == "|": return a | b
        if op == "^": return a ^ b
        if op == "&": return a & b
        if op == "<<": return a << b
        if op == ">>": return a >> b
        if op == "+": return a + b
        if op == "-": return a - b
        if op == "*": return a * b
        if op == "/": return a // b
        if op == "%": return a % b
        raise ValueError(op)

    def unary(self):
        t = self.take()
        if t is None:
            raise ValueError("unexpected end of expression")
        if t == "<":
            return self.unary() & 0xFF
        if t == ">":
            return (self.unary() >> 8) & 0xFF
        if t == "-":
            return -self.unary()
        if t == "~":
            return ~self.unary()
        if t == "!":
            return int(not self.unary())
        if t == "(":
            v = self.expr(0)
            if self.take() != ")":
                raise ValueError("missing )")
            return v
        if t.startswith("$"):
            return int(t[1:], 16)
        if t.startswith("0x"):
            return int(t[2:], 16)
        if t.startswith("%"):
            return int(t[1:], 2)
        if t.isdigit():
            return int(t)
        if t.startswith("'") and len(t) == 3:
            return ord(t[1])
        if t == "BANK" and self.peek() == "(":
            self.take()
            name = self.take()
            if self.take() != ")":
                raise ValueError("missing ) after BANK")
            if name not in LABEL_BANKS:
                raise Unknown(name)
            return LABEL_BANKS[name]
        if t in ("HIGH", "LOW") and self.peek() == "(":
            self.take()
            v = self.expr(0)
            if self.take() != ")":
                raise ValueError("missing ) after %s" % t)
            return (v >> 8) & 0xFF if t == "HIGH" else v & 0xFF
        if t in FUNCS:
            nargs, body = FUNCS[t]
            if self.take() != "(":
                raise ValueError("%s used without an argument list" % t)
            args = [self.expr(0)]
            while self.peek() == ",":
                self.take()
                args.append(self.expr(0))
            if self.take() != ")" or len(args) != nargs:
                raise ValueError("bad call to %s (expects %d args)" % (t, nargs))
            scope = dict(self.syms)
            scope.update({"%s%d" % (FUNC_ARG, i + 1): v for i, v in enumerate(args)})
            return evaluate(body, scope)
        if t in self.syms:
            return self.syms[t]
        raise Unknown(t)


def evaluate(text, syms):
    return ExprParser(text, syms).parse()


# ---------------------------------------------------------------------------
# Source loading
# ---------------------------------------------------------------------------
COMMENT_RE = re.compile(r'^((?:[^;"]|"[^"]*")*)')


def strip_comment(line):
    m = COMMENT_RE.match(line)
    return m.group(1).rstrip() if m else line.rstrip()


def load_lines(src_path):
    """Expand .include recursively. Yields (file, lineno, text)."""
    base = os.path.dirname(os.path.abspath(src_path))
    root = base

    def expand(path, depth=0):
        if depth > 8:
            raise RuntimeError("include too deep")
        with open(path, "r", encoding="latin-1") as fh:
            for n, raw in enumerate(fh, 1):
                line = raw.rstrip("\r\n")
                # A label may share the line with .include (e.g. "_C000:  .include ...").
                lm = re.match(r'^\s*([A-Za-z_.][A-Za-z0-9_.]*):?\s+(\.include\s+"[^"]+".*)$', line)
                if not lm:
                    lm = re.match(r'^\s*([A-Za-z_.][A-Za-z0-9_.]*):\s*(\.include\s+"[^"]+".*)$', line)
                if lm:
                    yield (os.path.relpath(path, root), n, lm.group(1) + ":")
                    line = lm.group(2)
                m = re.match(r'^\s*\.include\s+"([^"]+)"', line)
                if m:
                    inc = os.path.join(root, m.group(1))
                    # The original assembler appends .asm to extension-less includes.
                    if not os.path.exists(inc) and os.path.exists(inc + ".asm"):
                        inc += ".asm"
                    yield from expand(inc, depth + 1)
                    continue
                yield (os.path.relpath(path, root), n, line)

    yield from expand(src_path)


# ---------------------------------------------------------------------------
# Item model
# ---------------------------------------------------------------------------
class Item:
    __slots__ = ("kind", "file", "line", "text", "bank", "addr", "mn", "operand",
                 "exprs", "size", "name", "expr", "emit", "fwd")

    def __init__(self, kind, file, line, text, bank=None):
        self.kind = kind
        self.file = file
        self.line = line
        self.text = text
        self.bank = bank
        self.addr = None
        self.mn = None
        self.operand = None
        self.exprs = None
        self.size = None
        self.name = None
        self.expr = None
        self.emit = None


LABEL_RE = re.compile(r"^\s*([A-Za-z_.][A-Za-z0-9_.]*):(.*)$")
DEF_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$")
MACRO_START_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*):?\s+\.macro\b")


def split_byte_list(text):
    """Split a .byte/.word operand list on commas that are not inside quotes."""
    parts, cur, inq = [], "", False
    depth = 0
    for ch in text:
        if ch == '"':
            inq = not inq
        elif not inq and ch == "(":
            depth += 1
        elif not inq and ch == ")":
            depth -= 1
        if ch == "," and not inq and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


# Macros the translator handles by name (or ignores) are never expanded generically.
NATIVE_MACROS = set(IGNORED_MACROS) | {"vaddr"} | set(MACRO_BRANCH) | set(MACRO_ALU)
MAX_MACRO_DEPTH = 8


def collect_macros(lines):
    """Remove .macro definitions from the stream; return (name -> body lines, rest)."""
    macros, keep, cur = {}, [], None
    for item in lines:
        raw = item[2]
        if cur is not None:
            if re.match(r"^\s*\.endm\b", raw):
                cur = None
            else:
                macros[cur].append(raw)
            continue
        mm = MACRO_START_RE.match(raw)
        if mm:
            cur = mm.group(1)
            macros[cur] = []
            continue
        keep.append(item)
    return macros, keep


def expand_macros(lines, macros, depth=0):
    """Expand user macro invocations (name args...) by substituting \\N with the N-th argument."""
    if depth > MAX_MACRO_DEPTH:
        raise RuntimeError("macro expansion too deep")
    out = []
    for file, lineno, raw in lines:
        line = strip_comment(raw)
        label = ""
        m = LABEL_RE.match(line)
        while m:
            label += m.group(1) + ":"
            line = m.group(2)
            m = LABEL_RE.match(line)
        parts = line.strip().split(None, 1)
        if not parts or parts[0] not in macros or parts[0] in NATIVE_MACROS:
            out.append((file, lineno, raw))
            continue
        if label:
            out.append((file, lineno, label))
        args = split_byte_list(parts[1]) if len(parts) > 1 else []
        body = []
        for text in macros[parts[0]]:
            body.append((file, lineno, re.sub(
                r"\\(\d+)",
                lambda g: args[int(g.group(1)) - 1] if 0 < int(g.group(1)) <= len(args) else g.group(0),
                text)))
        out.extend(expand_macros(body, macros, depth + 1))
    return out


def parse_source(src_path):
    items = []
    defs = {}
    macros, lines = collect_macros(load_lines(src_path))
    for file, lineno, raw in expand_macros(lines, macros):
        line = strip_comment(raw)
        if not line.strip():
            continue
        fm = FUNC_RE.match(line)
        if fm:
            body = fm.group(2)
            nargs = max([int(d) for d in re.findall(r"\\(\d)", body)] or [1])
            for i in range(1, nargs + 1):
                body = body.replace("\\%d" % i, " %s%d " % (FUNC_ARG, i))
            FUNCS[fm.group(1)] = (nargs, body)
            continue
        # Label(s) at the start of the line
        m = LABEL_RE.match(line)
        while m:
            items.append(Item("label", file, lineno, m.group(1)))
            items[-1].name = m.group(1)
            line = m.group(2)
            m = LABEL_RE.match(line)
        body = line.strip()
        if not body:
            continue
        # NESASM also accepts a column-0 label with no colon, e.g. "Name  .byte ..."
        bp = body.split(None, 1)
        if not line[:1].isspace() and len(bp) == 2 and re.match(r"\.(byte|word|ds)\b", bp[1], re.I):
            items.append(Item("label", file, lineno, bp[0]))
            items[-1].name = bp[0]
            body = bp[1]
        elif (not line[:1].isspace() and len(bp) == 1 and bp[0].upper() not in M
              and not bp[0].startswith(".") and not DEF_RE.match(body)):
            # A bare column-0 name on its own line is a label.
            items.append(Item("label", file, lineno, bp[0]))
            items[-1].name = bp[0]
            continue
        dm = DEF_RE.match(body)
        if dm and not body.startswith("."):
            it = Item("def", file, lineno, body)
            it.name = dm.group(1)
            it.expr = dm.group(2)
            items.append(it)
            continue
        if body.split(None, 1)[0].lower() in ("org", "bank", "byte", "word", "ds"):
            body = "." + body  # NESASM also takes these directives without the dot
        if body.startswith("."):
            parts = body.split(None, 1)
            d = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ""
            if d == ".bank":
                it = Item("bank", file, lineno, body)
                it.expr = arg
                items.append(it)
            elif d == ".org":
                it = Item("org", file, lineno, body)
                it.expr = arg
                items.append(it)
            elif d == ".byte":
                it = Item("bytes", file, lineno, body)
                it.exprs = split_byte_list(arg)
                items.append(it)
            elif d == ".word":
                it = Item("words", file, lineno, body)
                it.exprs = split_byte_list(arg)
                items.append(it)
            elif d == ".ds":
                it = Item("ds", file, lineno, body)
                it.expr = arg
                items.append(it)
            elif d in (".incchr", ".inesprg", ".ineschr", ".inesmap", ".inesmir",
                       ".data", ".code", ".zp", ".endm", ".macro", ".equ"):
                continue
            else:
                raise RuntimeError("unhandled directive %s at %s:%d" % (d, file, lineno))
            continue
        # Instruction or macro
        parts = body.split(None, 1)
        mn = parts[0]
        operand = parts[1].strip() if len(parts) > 1 else ""
        up = mn.upper()
        if mn == "vaddr" or up == "VADDR":
            it = Item("vaddr", file, lineno, body)
            it.expr = operand.rstrip(", \t")  # one-argument macro; NESASM tolerates a stray comma
            items.append(it)
            continue
        if mn in IGNORED_MACROS:
            if mn == "DMCAlign":
                it = Item("dmcalign", file, lineno, body)
                items.append(it)
            continue
        if mn.startswith("."):
            raise RuntimeError("unhandled directive %s at %s:%d" % (mn, file, lineno))
        it = Item("instr", file, lineno, body)
        it.mn = up
        it.operand = operand
        items.append(it)
    return items


# ---------------------------------------------------------------------------
# Operand decoding
# ---------------------------------------------------------------------------
def decode_operand(mn, operand):
    """Return (mode, expr_text, force_zp) for an instruction operand.

    mode is one of: impl, acc, imm, zp, zpx, zpy, abs, absx, absy, indx,
    indy, ind, rel. For zp-capable modes the caller may downgrade to abs
    when the value does not fit in a byte.
    """
    if operand == "":
        return ("impl", None, False)
    if mn in ("ASL", "LSR", "ROL", "ROR") and operand.upper() == "A":
        return ("acc", None, False)
    if operand.startswith("#"):
        return ("imm", operand[1:].strip(), False)
    if mn in ("BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"):
        return ("rel", operand, False)
    force_zp = False
    op = operand
    if op.startswith("<"):
        force_zp = True
        op = op[1:]
    if op.startswith("["):  # NESASM writes [zp],Y and [zp,X] for the (zp),Y and (zp,X) modes
        op = "(" + op[1:].replace("]", ")")
    m = re.match(r"^\((.*),\s*[Xx]\)\s*$", op)
    if m:
        return ("indx", m.group(1).strip(), force_zp)
    m = re.match(r"^\((.*)\)\s*,\s*[Yy]\s*$", op)
    if m:
        return ("indy", m.group(1).strip(), force_zp)
    m = re.match(r"^\((.*)\)\s*$", op)
    if m:
        return ("ind", m.group(1).strip(), force_zp)
    m = re.match(r"^(.*?)\s*,\s*([XxYy])\s*$", op)
    if m:
        idx = m.group(2).upper()
        return ("zp" + idx.lower(), m.group(1).strip(), force_zp)
    return ("abs", op.strip(), force_zp)


# NESASM sizes an operand that refers to a symbol not yet defined (in source
# order) as absolute. _SEEN holds the symbols defined so far in the current
# layout pass; _NOZP is set while sizing/encoding an instruction that has such
# a forward reference.
_SEEN = set()
_NOZP = False
_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def has_forward_ref(operand):
    for name in _IDENT.findall(operand or ""):
        if name in ("HIGH", "LOW", "BANK", "A", "X", "Y"):
            continue
        if name not in _SEEN:
            return True
    return False


def resolve_mode(mn, mode, value, force_zp, expr=None):
    """Choose the final addressing mode (zp vs abs) like NESASM does.

    NESASM uses zero page only when the operand is forced with '<', or when it
    is a literal below $100. A symbol without '<' is always absolute, even if
    its value is below $100. A forward reference is also always absolute.
    """
    symbolic = any(n not in ("HIGH", "LOW", "BANK") for n in _IDENT.findall(expr or ""))
    small = value is not None and value < 0x100
    if mode in ("zp", "zpx", "zpy"):
        if not _NOZP and (force_zp or (mode == "zp" and small and not symbolic)):
            if mode in M.get(mn, {}):
                return mode
        return ZP_FALLBACK[mode]
    if mode == "abs":
        if not _NOZP and (force_zp or (small and not symbolic)) and "zp" in M.get(mn, {}):
            return "zp"
        return "abs"
    return mode


# ---------------------------------------------------------------------------
# Pass 1 / 2: addresses and symbols
# ---------------------------------------------------------------------------
def byte_size_of(it):
    if it.kind == "bytes":
        n = 0
        for e in it.exprs:
            if e.startswith('"'):
                n += len(e) - 2
            else:
                n += 1
        return n
    return None


def layout(items, syms):
    """Assign addresses to all items. Returns (ok, unknowns)."""
    global _NOZP
    bank = None
    pc = 0
    unknown = set()
    _SEEN.clear()
    for it in items:
        it.bank = bank
        if it.kind == "bank":
            bank = evaluate(it.expr, syms)
            it.bank = bank
            continue
        if it.kind == "org":
            pc = evaluate(it.expr, syms)
            it.addr = pc
            continue
        if it.kind == "label":
            syms[it.name] = pc
            LABEL_BANKS[it.name] = bank
            _SEEN.add(it.name)
            it.addr = pc
            continue
        if it.kind == "def":
            try:
                syms[it.name] = evaluate(it.expr, syms)
                _SEEN.add(it.name)
            except Unknown as u:
                unknown.add(str(u))
            it.addr = pc
            continue
        it.addr = pc
        if it.kind == "bytes":
            it.size = byte_size_of(it)
        elif it.kind == "words":
            it.size = 2 * len(it.exprs)
        elif it.kind == "ds":
            try:
                it.size = evaluate(it.expr, syms)
            except Unknown as u:
                unknown.add(str(u))
                it.size = 0
        elif it.kind == "vaddr":
            it.size = 2
        elif it.kind == "dmcalign":
            it.size = ((pc + 0x3F) & 0xFFC0) - pc
        elif it.kind == "instr":
            it.fwd = has_forward_ref(it.operand)
            _NOZP = it.fwd
            try:
                it.size = instr_size(it, syms, unknown)
            finally:
                _NOZP = False
        pc = (pc + it.size) & 0xFFFF if it.size is not None else pc
    return unknown


def instr_size(it, syms, unknown):
    mn = it.mn
    if mn in MACRO_BRANCH:
        return 2
    if mn in MACRO_ALU:
        # ADD/SUB expand to CLC/SEC + ADC/SBC with operand
        mode, expr, force = decode_operand("ADC", it.operand)
        return 1 + instr_size_mode("ADC" if mn == "ADD" else "SBC", it.operand, syms, unknown)
    if mn == "NEG":
        return 5  # EOR #$FF (2) + CLC (1) + ADC #$01 (2)
    return instr_size_mode(mn, it.operand, syms, unknown)


def instr_size_mode(mn, operand, syms, unknown):
    mode, expr, force = decode_operand(mn, operand)
    if mode in ("impl", "acc"):
        return M[mn][mode][1] if mode in M.get(mn, {}) else 1
    if mode == "rel":
        return 2
    if mode == "imm":
        return 2
    value = None
    if expr is not None:
        try:
            value = evaluate(expr, syms)
        except Unknown as u:
            unknown.add(str(u))
    fm = resolve_mode(mn, mode, value, force, expr)
    if fm not in M.get(mn, {}):
        raise RuntimeError("no encoding for %s %s (mode %s)" % (mn, operand, fm))
    return M[mn][fm][1]


# ---------------------------------------------------------------------------
# Encoding (used for verification and to choose bytes)
# ---------------------------------------------------------------------------
def encode_instr(it, syms, pc):
    """Return (bytes, mode, target_addr, page_cross_penalty_possible)."""
    global _NOZP
    _NOZP = getattr(it, "fwd", False)
    try:
        return _encode_instr(it, syms, pc)
    finally:
        _NOZP = False


def _encode_instr(it, syms, pc):
    mn = it.mn
    if mn in MACRO_BRANCH:
        real = MACRO_BRANCH[mn]
        out = encode_instr_mode(real, it.operand, syms, pc)
        return out
    if mn in MACRO_ALU:
        real = "ADC" if mn == "ADD" else "SBC"
        pre = bytes([0x18 if mn == "ADD" else 0x38])
        body, mode, tgt = encode_instr_mode(real, it.operand, syms, pc + 1)
        return pre + body, mode, tgt
    if mn == "NEG":
        return bytes([0x49, 0xFF, 0x18, 0x69, 0x01]), "neg", None
    body, mode, tgt = encode_instr_mode(mn, it.operand, syms, pc)
    return body, mode, tgt


def encode_instr_mode(mn, operand, syms, pc):
    mode, expr, force = decode_operand(mn, operand)
    if mode == "impl":
        return bytes([M[mn]["impl"][0]]), "impl", None
    if mode == "acc":
        return bytes([M[mn]["acc"][0]]), "acc", None
    if mode == "rel":
        tgt = evaluate(operand, syms)
        opc = M[mn]["rel"][0]
        off = tgt - (pc + 2)
        if not (-128 <= off <= 127):
            raise RuntimeError("branch %s out of range (%d) at %04X" % (mn, off, pc))
        return bytes([opc, off & 0xFF]), "rel", tgt
    if mode == "imm":
        v = evaluate(expr, syms) & 0xFF
        return bytes([M[mn]["imm"][0], v]), "imm", None
    value = evaluate(expr, syms) if expr is not None else None
    fm = resolve_mode(mn, mode, value, force, expr)
    if fm not in M.get(mn, {}):
        raise RuntimeError("no encoding for %s %s (mode %s)" % (mn, operand, fm))
    opc, size, _ = M[mn][fm]
    if fm in ("zp", "zpx", "zpy", "indx", "indy"):
        v = value & 0xFF
        return bytes([opc, v]), fm, value
    if fm == "ind":
        return bytes([opc, value & 0xFF, (value >> 8) & 0xFF]), fm, value
    return bytes([opc, value & 0xFF, (value >> 8) & 0xFF]), fm, value


# ---------------------------------------------------------------------------
# Verification against ROM
# ---------------------------------------------------------------------------
def rom_offset(bank, addr):
    return 16 + bank * 0x2000 + (addr & 0x1FFF)


def verify(items, syms, rom):
    mismatches = []
    checked = 0
    prev_instr = None
    for it in items:
        before, prev_instr = prev_instr, (it if it.kind == "instr" else prev_instr)
        if it.bank is None:
            # Data before the first .bank has no ROM location: it cannot be checked or emitted.
            # RAM .ds reservations are not ROM data, so they are skipped.
            if it.kind in ("instr", "bytes", "words", "vaddr"):
                mismatches.append(("unbanked", it, None, None))
            continue
        if it.kind == "instr":
            data, mode, tgt = encode_instr(it, syms, it.addr)
            it.emit = (data, mode, tgt)
            if it.size is not None and len(data) != it.size:
                mismatches.append(("size", it, len(data), it.size))
                continue
            if is_gate(it):
                # Deliberate deviation (see GATE_TARGETS): not compared with the ROM bytes,
                # but the test it guards must be the instruction before it.
                if before is None or not before.operand or "SndCur_" not in before.operand:
                    mismatches.append(("gate", it, None, None))
                GATED_ITEMS.append(it)
                continue
            expect = rom[rom_offset(it.bank, it.addr):rom_offset(it.bank, it.addr) + len(data)]
            checked += 1
            if bytes(expect) != data:
                mismatches.append(("bytes", it, data, bytes(expect)))
        elif it.kind == "bytes":
            pos = it.addr
            vals = bytearray()
            for e in it.exprs:
                if e.startswith('"'):
                    vals += e[1:-1].encode("latin-1")
                else:
                    vals.append(evaluate(e, syms) & 0xFF)
            expect = rom[rom_offset(it.bank, it.addr):rom_offset(it.bank, it.addr) + len(vals)]
            checked += 1
            if bytes(expect) != bytes(vals):
                mismatches.append(("data", it, bytes(vals), bytes(expect)))
        elif it.kind == "words":
            vals = bytearray()
            for e in it.exprs:
                v = evaluate(e, syms) & 0xFFFF
                vals += bytes([v & 0xFF, v >> 8])
            expect = rom[rom_offset(it.bank, it.addr):rom_offset(it.bank, it.addr) + len(vals)]
            checked += 1
            if bytes(expect) != bytes(vals):
                mismatches.append(("word", it, bytes(vals), bytes(expect)))
        elif it.kind == "vaddr":
            v = evaluate(it.expr, syms) & 0xFFFF
            vals = bytes([(v >> 8) & 0xFF, v & 0xFF])
            expect = rom[rom_offset(it.bank, it.addr):rom_offset(it.bank, it.addr) + 2]
            checked += 1
            if bytes(expect) != vals:
                mismatches.append(("vaddr", it, vals, bytes(expect)))
        elif it.kind == "ds" and it.size:
            expect = rom[rom_offset(it.bank, it.addr):rom_offset(it.bank, it.addr) + it.size]
            checked += 1
            if any(expect):
                # NESASM pads .ds with zeros; anything else is a mismatch.
                mismatches.append(("ds", it, b"\x00" * it.size, bytes(expect)))
    return checked, mismatches


# ---------------------------------------------------------------------------
# C++ code generation
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Sound ownership and deliberate deviations from the ROM
# ---------------------------------------------------------------------------
# Music and sound effects drive two separate APU voice sets (Machine::apu and
# Machine::sfx_apu). Generated code tags each APU write with the owner in
# m.snd_owner. A JSR into one of the routines below sets the owner, and the
# matching RTS restores the caller's owner through m.snd_sav (indexed by S).
# Other calls inherit the caller's owner, so helpers shared by music and effects
# (e.g. Sound_Sq1_NoteOn) write for whoever called them.
SND_MUSIC, SND_SFX, SND_BOTH = 0, 1, 2
SND_ROUTINE_OWNER = {
    "Sound_Engine_Begin": SND_SFX,
    "Sound_PlayMusic": SND_MUSIC,
    "Sound_PlayLevel1": SND_SFX,
    "Sound_PlayLevel2": SND_SFX,
    "Sound_PlayPlayer": SND_SFX,
    "Sound_PlayMapSounds": SND_SFX,
}
# Filled in main() from the resolved symbols.
SND_OWNER_BY_ADDR = {}
# Every level load funnels through Level_Initialize (one JSR site, bank 08, prg008.asm:293).
# The routine runs every frame while a level is active ("initialize level if needed") and
# returns early once Player_StartX is set; the real (re)initialisation body begins at its
# PRG008_A242 label. Hooking the body (not the JSR) fires once per genuine level load rather
# than once per frame, and it also covers re-entries that arrive after the routine's own
# early-out. There the game re-initialises only the music voices, so the sfx set would
# otherwise carry channel state from the previous level into the new one (a ringing channel,
# an unfinished envelope, a looping DMC sample) with nothing rewriting its registers under
# the new level's music. The callers still get nothing: the reset lives at Machine::
# reset_sfx_apu, invoked from the body's first instruction.
LEVEL_INIT_BODY_ADDR = None    # first instruction of the init body (PRG008_A242)
LEVEL_INIT_BODY_FILE = None    # source file leaf owning that instruction
# [start, end) of the pause routine. Its $4015 writes reach both voice sets, as the
# original pause silenced every channel.
SND_PAUSE_RANGE = [0, 0]

# Deliberate deviation from the ROM. In the music code (prg031.asm), each of these
# BNE branches is a gate: when a sound effect uses the same channel (SndCur_Player /
# SndCur_Map for square 1, SndCur_Level1 for square 2), the music skips its note or
# envelope update, so the track stops under the effect. The gate is removed here, so
# music keeps playing. Each target must be preceded by a test of SndCur_*.
GATE_TARGETS = ("PRG031_E574", "PRG031_E5A7", "PRG031_E625", "PRG031_E660")
GATED_ITEMS = []  # filled by verify(), reported by main()


def is_gate(it):
    return (it.kind == "instr" and it.mn == "BNE" and it.operand in GATE_TARGETS
            and os.path.basename(it.file) == "prg031.asm")


def hexa(v):
    return "0x%04X" % (v & 0xFFFF)


def read_expr(mode, val_expr, pen):
    """Return a C++ expression reading the operand value (value-producing)."""
    if mode == "imm":
        return "0x%02X" % (val_expr & 0xFF)
    if mode == "zp":
        return "m.rd(%s)" % hexa(val_expr & 0xFF)
    if mode == "zpx":
        return "m.rd(m.zpx(%s))" % ("0x%02X" % (val_expr & 0xFF))
    if mode == "zpy":
        return "m.rd(m.zpy(%s))" % ("0x%02X" % (val_expr & 0xFF))
    if mode == "abs":
        return "m.rd(%s)" % hexa(val_expr)
    if mode == "absx":
        return "m.rd(m.eaAbsX(%s, %s))" % (hexa(val_expr), "true" if pen else "false")
    if mode == "absy":
        return "m.rd(m.eaAbsY(%s, %s))" % (hexa(val_expr), "true" if pen else "false")
    if mode == "indx":
        return "m.rd(m.eaIndX(0x%02X))" % (val_expr & 0xFF)
    if mode == "indy":
        return "m.rd(m.eaIndY(0x%02X, %s))" % (val_expr & 0xFF, "true" if pen else "false")
    raise RuntimeError("read mode %s" % mode)


def ea_expr(mode, val_expr):
    """Return a C++ expression for the effective address (no penalty)."""
    if mode == "zp":
        return "0x%02X" % (val_expr & 0xFF)
    if mode == "zpx":
        return "m.zpx(0x%02X)" % (val_expr & 0xFF)
    if mode == "zpy":
        return "m.zpy(0x%02X)" % (val_expr & 0xFF)
    if mode == "abs":
        return hexa(val_expr)
    if mode == "absx":
        return "m.eaAbsX(%s, false)" % hexa(val_expr)
    if mode == "absy":
        return "m.eaAbsY(%s, false)" % hexa(val_expr)
    if mode == "indx":
        return "m.eaIndX(0x%02X)" % (val_expr & 0xFF)
    if mode == "indy":
        return "m.eaIndY(0x%02X, false)" % (val_expr & 0xFF)
    raise RuntimeError("ea mode %s" % mode)


def base_cycles(mn, mode):
    if mode == "rel":
        return 2
    return M[mn][mode][2]


class BankEmitter:
    def __init__(self, labels):
        self.labels = labels  # set of addresses with a label in this bank
        self.lines = []

    def out(self, s):
        self.lines.append(s)

    def target(self, addr):
        """Emit a transfer to addr from within the bank function."""
        if addr in self.labels:
            self.out("if (m.cyc >= m.evt) return %s; goto L_%04X;" % (hexa(addr), addr))
        else:
            self.out("return %s;" % hexa(addr))


def emit_instr(be, it, data, mode, tgt, records=None):
    mn = it.mn
    a = it.addr
    cyc_label = "L_%04X" % a
    comment = "// %s:%d  %s" % (it.file, it.line, it.text.replace("*/", "* /"))
    be.out("%s:  %s" % (cyc_label, comment))
    # Return to the dispatcher before any instruction once an event (line start, MMC3 clock,
    # pending IRQ) is due, so interrupts are taken on the cycle they occur, not at the next label.
    be.out("if (m.cyc >= m.evt) return %s;" % hexa(a))
    if is_gate(it):
        be.out("m.cyc += 2;  // gate removed: music ignores sound-effect state (GATE_TARGETS)")
        return
    if mn in MACRO_BRANCH:
        real = MACRO_BRANCH[mn]
        return emit_branch(be, real, a, tgt, records)
    if mn in MACRO_ALU:
        real = "ADC" if mn == "ADD" else "SBC"
        # The CLC/SEC prefix is one byte; emit as a single C++ sequence.
        be.out("m.cyc += 2; m.p &= ~0x01;" if mn == "ADD" else "m.cyc += 2; m.p |= 0x01;")
        emit_alu_real(be, real, it)
        return
    if mn == "NEG":
        be.out("m.cyc += 6; m.a = (uint8_t)(~m.a); m.p &= ~0x01; m.adc(0x01);")
        return
    if mn in ("BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"):
        return emit_branch(be, mn, a, tgt, records)
    emit_real(be, mn, mode, it, data, tgt)


def emit_alu_real(be, real, it):
    mode, expr, force = decode_operand(real, it.operand)
    value = evaluate(expr, SYMS_REF[0]) if expr is not None else None
    fm = resolve_mode(real, mode, value, force, expr)
    cy = base_cycles(real, fm)
    be.out("m.cyc += %d;" % cy)
    if fm == "imm":
        be.out("m.%s(0x%02X);" % ("adc" if real == "ADC" else "sbc", value & 0xFF))
    else:
        be.out("m.%s(%s);" % ("adc" if real == "ADC" else "sbc", read_expr(fm, value, True)))


def emit_branch(be, mn, a, tgt, records=None):
    cond = {
        "BCC": "!(m.p & 0x01)", "BCS": "(m.p & 0x01)",
        "BEQ": "(m.p & 0x02)", "BNE": "!(m.p & 0x02)",
        "BMI": "(m.p & 0x80)", "BPL": "!(m.p & 0x80)",
        "BVC": "!(m.p & 0x40)", "BVS": "(m.p & 0x40)",
    }[mn]
    fall = (a + 2) & 0xFFFF
    page = "((%s ^ %s) >> 8) ? 1 : 0" % ("0x%04X" % fall, hexa(tgt))
    be.out("m.cyc += 2;")
    be.out("if (%s) { m.cyc += 1 + (%s); " % (cond, page))
    # A taken branch that skips sprite Y-byte stores is a drawer's off-screen culling: it
    # leaves the piece out of the frame. Report the Y value it would have stored (still in A)
    # so the widescreen pass can draw the piece in the columns the frame is not wide enough
    # to show. The record costs no emulated cycles and does not touch the staging buffer.
    for rec in records or ():
        be.out("  m.spr_y_record(%s, m.a);" % rec)
    # taken branch
    if tgt in be.labels:
        be.out("  if (m.cyc >= m.evt) return %s; goto L_%04X; }" % (hexa(tgt), tgt))
    else:
        be.out("  return %s; }" % hexa(tgt))


SPR_PAGE = 0x0200
SPR_SLOTS = 64


def spr_y_store_ea(it):
    """Effective-address expression if this instruction stores a sprite Y byte, else None.

    Sprite pieces are 4 bytes ($0200-$02FF, Y/pattern/attributes/X); the Y byte is the only
    one writers skip to hide a piece. Y is the region offset the game adds, so the static
    base's low two bits decide which byte is stored regardless of Y's runtime value."""
    if it.kind != "instr" or it.mn != "STA" or not it.emit:
        return None
    data, mode, _tgt = it.emit
    if mode not in ("abs", "absy", "absx") or len(data) < 3:
        return None
    val = data[1] | (data[2] << 8)
    if not (SPR_PAGE <= val < SPR_PAGE + 4 * SPR_SLOTS) or (val & 3):
        return None
    return ea_expr(mode, val)


def cull_records(items):
    """Map branch address -> Y-store effective addresses skipped when that branch is taken.

    Only runs made up entirely of sprite Y-byte stores qualify, so A still holds the Y value
    at the branch and every skipped store would have written it."""
    out = {}
    by_addr = {it.addr: it for it in items if it.kind == "instr"}
    for it in items:
        if it.kind != "instr" or it.size != 2 or not it.mn or not it.emit:
            continue
        if MACRO_BRANCH.get(it.mn) is None and it.mn not in (
                "BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"):
            continue
        _data, _mode, tgt = it.emit
        if tgt <= it.addr:
            continue
        run = []
        a = it.addr + 2
        while a < tgt:
            ins = by_addr.get(a)
            ea = spr_y_store_ea(ins) if ins is not None else None
            if ea is None:
                run = None
                break
            run.append(ea)
            a += ins.size
        if run:
            out[it.addr] = run
    return out


def emit_real(be, mn, mode, it, data, tgt):
    # Compute operand value from encoded bytes (verified already).
    val = None
    if mode in ("imm", "zp", "zpx", "zpy", "indx", "indy"):
        val = data[1]
    elif mode in ("abs", "absx", "absy", "ind"):
        val = data[1] | (data[2] << 8)
    cy = base_cycles(mn, mode)
    if mn in ("JSR",):
        owner = SND_OWNER_BY_ADDR.get(val)
        be.out("m.snd_sav[m.s] = m.snd_owner;" + ("" if owner is None else " m.snd_owner = %d;" % owner))
        be.out("m.cyc += %d; m.push16(0x%04X);" % (cy, (it.addr + 2) & 0xFFFF))
        be.out("return %s;" % hexa(val))
        return
    if mn == "JMP":
        be.out("m.cyc += %d;" % cy)
        if mode == "ind":
            be.out("{ uint16_t t = m.jmpInd(%s); return t; }" % hexa(val))
        else:
            be.target(val)
        return
    if mn == "RTS":
        be.out("m.cyc += %d; { uint16_t r = (uint16_t)(m.pull16() + 1); m.snd_owner = m.snd_sav[m.s]; return r; }" % cy)
        return
    if mn == "RTI":
        be.out("m.cyc += %d; m.p = (uint8_t)((m.pull() & ~0x10) | 0x20); "
               "{ uint16_t r = m.pull16(); return r; }" % cy)
        return
    if mn == "BRK":
        be.out("m.cyc += %d; m.brk(); return m.vectorIrq();" % cy)
        return
    if mn == "NOP":
        be.out("m.cyc += %d;" % cy)
        return
    if mn == "TAX":
        be.out("m.cyc += %d; m.x = m.a; m.nz(m.x);" % cy); return
    if mn == "TAY":
        be.out("m.cyc += %d; m.y = m.a; m.nz(m.y);" % cy); return
    if mn == "TXA":
        be.out("m.cyc += %d; m.a = m.x; m.nz(m.a);" % cy); return
    if mn == "TYA":
        be.out("m.cyc += %d; m.a = m.y; m.nz(m.a);" % cy); return
    if mn == "TSX":
        be.out("m.cyc += %d; m.x = m.s; m.nz(m.x);" % cy); return
    if mn == "TXS":
        be.out("m.cyc += %d; m.s = m.x;" % cy); return
    if mn == "INX":
        be.out("m.cyc += %d; m.x = m.nzv((uint8_t)(m.x + 1));" % cy); return
    if mn == "INY":
        be.out("m.cyc += %d; m.y = m.nzv((uint8_t)(m.y + 1));" % cy); return
    if mn == "DEX":
        be.out("m.cyc += %d; m.x = m.nzv((uint8_t)(m.x - 1));" % cy); return
    if mn == "DEY":
        be.out("m.cyc += %d; m.y = m.nzv((uint8_t)(m.y - 1));" % cy); return
    if mn in ("CLC",):
        be.out("m.cyc += %d; m.p &= ~0x01;" % cy); return
    if mn in ("SEC",):
        be.out("m.cyc += %d; m.p |= 0x01;" % cy); return
    if mn in ("CLI",):
        be.out("m.cyc += %d; m.p &= ~0x04; m.kick_irq();" % cy); return
    if mn in ("SEI",):
        be.out("m.cyc += %d; m.p |= 0x04;" % cy); return
    if mn in ("CLD",):
        be.out("m.cyc += %d; m.p &= ~0x08;" % cy); return
    if mn in ("SED",):
        be.out("m.cyc += %d; m.p |= 0x08;" % cy); return
    if mn in ("CLV",):
        be.out("m.cyc += %d; m.p &= ~0x40;" % cy); return
    if mn == "PHA":
        be.out("m.cyc += %d; m.push(m.a);" % cy); return
    if mn == "PHP":
        be.out("m.cyc += %d; m.push((uint8_t)(m.p | 0x30));" % cy); return
    if mn == "PLA":
        be.out("m.cyc += %d; m.a = m.pull(); m.nz(m.a);" % cy); return
    if mn == "PLP":
        be.out("m.cyc += %d; m.p = (uint8_t)((m.pull() & ~0x10) | 0x20); m.kick_irq();" % cy); return

    # Memory-operand instructions
    if mn in ("LDA", "LDX", "LDY"):
        reg = {"LDA": "a", "LDX": "x", "LDY": "y"}[mn]
        pen = mode in ("absx", "absy", "indy")
        be.out("m.cyc += %d; m.%s = %s; m.nz(m.%s);" % (cy, reg, read_expr(mode, val, pen), reg))
        return
    if mn in ("STA", "STX", "STY"):
        reg = {"STA": "a", "STX": "x", "STY": "y"}[mn]
        if (it.addr == LEVEL_INIT_BODY_ADDR
                and os.path.basename(it.file) == LEVEL_INIT_BODY_FILE):
            # First instruction of the level-init body (PRG008_A242): a genuine level load
            # starts here every time (fresh levels, death re-entry, sub-area changes). The
            # body is only ever entered through Level_Initialize itself - the disassembly
            # has no branch into it and no other routine calls it - so an APU-re-run sandbox
            # pass (widescreen drawer re-runs) can never execute this instruction to skip a
            # rollback; if a source refresh ever adds such a path, it must also guard this
            # sink so it does not fire inside a re-run.
            be.out("m.reset_sfx_apu();  // level load: the sfx voice set starts clean")
        if mode == "abs" and val == 0x4015 and SND_PAUSE_RANGE[0] <= it.addr < SND_PAUSE_RANGE[1]:
            be.out("m.cyc += %d; { uint8_t keep = m.snd_owner; m.snd_owner = %d; m.wr(0x4015, m.%s); "
                   "m.snd_owner = keep; }" % (cy, SND_BOTH, reg))
            return
        # Sprite Y bytes also feed the widescreen pass's record of what each drawer computed
        # (see spr_y_store_ea); the write itself is unchanged.
        spr_ea = spr_y_store_ea(it)
        if spr_ea is not None:
            be.out("m.cyc += %d; m.spr_y_store(%s, m.a);" % (cy, spr_ea))
            return
        be.out("m.cyc += %d; m.wr(%s, m.%s);" % (cy, ea_expr(mode, val), reg))
        return
    if mn in ("AND", "ORA", "EOR"):
        op = {"AND": "&=", "ORA": "|=", "EOR": "^="}[mn]
        pen = mode in ("absx", "absy", "indy")
        be.out("m.cyc += %d; m.a %s %s; m.nz(m.a);" % (cy, op, read_expr(mode, val, pen)))
        return
    if mn in ("ADC", "SBC"):
        pen = mode in ("absx", "absy", "indy")
        fn = "adc" if mn == "ADC" else "sbc"
        be.out("m.cyc += %d; m.%s(%s);" % (cy, fn, read_expr(mode, val, pen)))
        return
    if mn in ("CMP", "CPX", "CPY"):
        reg = {"CMP": "a", "CPX": "x", "CPY": "y"}[mn]
        pen = mode in ("absx", "absy", "indy")
        be.out("m.cyc += %d; m.cmp(m.%s, %s);" % (cy, reg, read_expr(mode, val, pen)))
        return
    if mn == "BIT":
        be.out("m.cyc += %d; m.bit(%s);" % (cy, read_expr(mode, val, False)))
        return
    if mn in ("ASL", "LSR", "ROL", "ROR", "INC", "DEC"):
        if mode == "acc":
            if mn in ("INC", "DEC"):
                raise RuntimeError("acc INC/DEC")
            fn = mn.lower()
            be.out("m.cyc += %d; m.a = m.%s(m.a);" % (cy, fn))
            return
        fn_map = {"ASL": "asl", "LSR": "lsr", "ROL": "rol", "ROR": "ror"}
        ea = ea_expr(mode, val)
        be.out("m.cyc += %d; { uint16_t ea = %s; uint8_t v = m.rd(ea); " % (cy, ea))
        if mn in fn_map:
            be.out("  v = m.%s(v); m.wr(ea, v); }" % fn_map[mn])
        elif mn == "INC":
            be.out("  v = m.nzv((uint8_t)(v + 1)); m.wr(ea, v); }")
        else:
            be.out("  v = m.nzv((uint8_t)(v - 1)); m.wr(ea, v); }")
        return
    raise RuntimeError("unsupported mnemonic %s in %s:%d" % (mn, it.file, it.line))


SYMS_REF = [None]
SPR_SITES = [0, 0]  # [branches over sprite Y stores, stores they skip]


def emit_bank(bank, origin, items):
    labels = set()
    for it in items:
        if it.kind == "instr":
            labels.add(it.addr)
    be = BankEmitter(labels)
    culls = cull_records(items)
    SPR_SITES[0] += len(culls)
    SPR_SITES[1] += sum(len(v) for v in culls.values())
    cases = sorted(labels)
    lines = []
    lines.append("// Generated by tools/translate/translate.py -- do not edit.")
    lines.append("// Source: SMB3 disassembly (PRG bank %d, origin $%04X).  Verified against ROM." % (bank, origin))
    lines.append('#include "runtime/machine.h"')
    lines.append("")
    lines.append("uint16_t bank_%02d(Machine& m, uint16_t entry) {" % bank)
    lines.append("  switch (entry) {")
    for c in cases:
        lines.append("    case 0x%04X: goto L_%04X;" % (c, c))
    lines.append("    default: return m.badEntry(entry, %d);" % bank)
    lines.append("  }")
    for it in items:
        if it.kind == "instr":
            data, mode, tgt = it.emit
            # Fresh emitter buffer per instruction, then splice
            sub = BankEmitter(labels)
            emit_instr(sub, it, data, mode, tgt, culls.get(it.addr))
            lines.extend("  " + l if not l.startswith("L_") else l for l in sub.lines)
    lines.append("  return %s;  // fall out of the bank window" % hexa(origin + 0x2000))
    lines.append("}")
    lines.append("")
    return "\n".join(lines), labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        rom = fh.read()
    if rom[:4] != b"NES\x1a":
        sys.exit("not an iNES file")
    prg_size = rom[4] * 0x4000
    if prg_size != 0x40000 or (rom[6] >> 4) != 4:
        sys.exit("ROM is not the expected SMB3 PRG1 layout (16x16KB PRG, MMC3)")

    items = parse_source(args.src)
    syms = {}
    # Iterate layout until symbols settle (forward references).
    for _ in range(8):
        before = dict(syms)
        unknown = layout(items, syms)
        if syms == before and not unknown:
            break
    unknown = layout(items, syms)
    if unknown:
        sys.exit("unresolved symbols: %s" % sorted(unknown)[:20])
    SYMS_REF[0] = syms

    for name, owner in SND_ROUTINE_OWNER.items():
        if name not in syms:
            sys.exit("sound routine %s not found in source" % name)
        SND_OWNER_BY_ADDR[syms[name]] = owner
    for name in ("SndPause", "Sound_Process"):
        if name not in syms:
            sys.exit("pause routine label %s not found in source" % name)
    SND_PAUSE_RANGE[:] = [syms["SndPause"], syms["Sound_Process"]]
    global LEVEL_INIT_BODY_ADDR, LEVEL_INIT_BODY_FILE
    if "Level_Initialize" not in syms:
        sys.exit("Level_Initialize label not found in source")
    # The hook sits at the first instruction of the init body, after the routine's own
    # early-out ("already initialised"): exactly where a genuine load starts, however the
    # caller arrived. The body label's instruction must be that unique store so a future
    # disassembly refresh cannot silently detach the hook from the load path.
    body_label = "PRG008_A242"
    if body_label not in syms:
        sys.exit("level-init body label %s not found in source" % body_label)
    LEVEL_INIT_BODY_ADDR = syms[body_label]
    cands = [it for it in items if it.kind == "instr" and it.addr == LEVEL_INIT_BODY_ADDR
             and it.mn == "STA" and it.operand == "Level_ChangeReset"]
    if len(cands) != 1:
        sys.exit("level-init body site not unique: %d STA Level_ChangeReset at $%04X"
                 % (len(cands), LEVEL_INIT_BODY_ADDR))
    LEVEL_INIT_BODY_FILE = os.path.basename(cands[0].file)

    checked, mismatches = verify(items, syms, rom)
    print("verified %d items against ROM; mismatches: %d" % (checked, len(mismatches)))
    print("deliberate deviations (music gates removed): %d of %d expected" % (len(GATED_ITEMS), len(GATE_TARGETS)))
    if len(GATED_ITEMS) != len(GATE_TARGETS):
        sys.exit("expected %d music gates, found %d" % (len(GATE_TARGETS), len(GATED_ITEMS)))
    print("level-init sfx reset: hooked at the init body ($%04X in %s), 1 site"
          % (LEVEL_INIT_BODY_ADDR, LEVEL_INIT_BODY_FILE))
    for kind, it, got, exp in mismatches[:25]:
        print("  MISMATCH %s %s:%d addr=$%04X bank=%s  %s\n    emitted=%s\n    rom    =%s" % (
            kind, it.file, it.line, it.addr or 0, it.bank, it.text,
            got.hex() if isinstance(got, (bytes, bytearray)) else got,
            exp.hex() if isinstance(exp, (bytes, bytearray)) else exp))
    if mismatches:
        sys.exit(1)

    os.makedirs(args.out, exist_ok=True)
    # Group instructions by bank
    banks = {}
    origins = {}
    for it in items:
        if it.kind == "org" and it.bank is not None:
            origins.setdefault(it.bank, it.addr)
    for it in items:
        if it.kind == "instr" and it.bank is not None:
            banks.setdefault(it.bank, []).append(it)
    table = []
    total = 0
    for bank in sorted(banks):
        origin = origins[bank]
        # Emit only the bank's own items (instructions + their labels are the same set)
        bank_items = [it for it in items if it.bank == bank and it.kind in ("instr",)]
        # all instruction addresses must lie inside the bank window
        for it in bank_items:
            if not (origin <= it.addr < origin + 0x2000):
                sys.exit("instruction %s:%d at $%04X outside bank %d window" % (it.file, it.line, it.addr, bank))
        text, _labels = emit_bank(bank, origin, bank_items)
        path = os.path.join(args.out, "bank_%02d.cpp" % bank)
        with open(path, "w") as fh:
            fh.write(text)
        total += len(bank_items)
        table.append((bank, origin))
    # Every bank file exists, empty if the bank has no code, so CMake can list
    # all 32 outputs up front instead of globbing at configure time.
    for b in range(32):
        if not any(t[0] == b for t in table):
            with open(os.path.join(args.out, "bank_%02d.cpp" % b), "w") as fh:
                fh.write("// Generated. Bank %d has no code.\n" % b)
    # Symbol header: the runtime's widescreen compose pass (src/runtime/wide.cpp) reads
    # game RAM and ROM tables by address. Emitting the layout's own symbols as constants
    # keeps that code honest when the disassembly moves something.
    RESERVED = {
        "and", "or", "not", "xor", "int", "auto", "class", "const", "double", "float",
        "for", "if", "else", "do", "while", "return", "struct", "union", "enum", "void",
        "char", "short", "long", "signed", "unsigned", "static", "extern", "register",
        "switch", "case", "default", "break", "continue", "goto", "sizeof", "typedef",
        "new", "delete", "this", "operator", "template", "namespace", "using", "try",
        "catch", "throw", "true", "false", "bool", "wchar_t", "inline", "virtual",
    }
    # ROM tables the wide pass reads directly out of the PRG image. Each gets its
    # byte offset in prg.bin (bank * $2000 + low 13 bits of the runtime address).
    ROM_LABELS = ["TileLayout_ByTileset", "TileLayoutPage_ByTileset"]
    for name in ROM_LABELS:
        if name not in syms or name not in LABEL_BANKS:
            sys.exit("wide-screen table %s is not a laid-out ROM label" % name)
    with open(os.path.join(args.out, "smb3_syms.h"), "w") as fh:
        fh.write("// Generated by tools/translate/translate.py -- do not edit.\n")
        fh.write("// Addresses from the verified disassembly layout (zero page, RAM, SRAM, ROM).\n")
        fh.write("#pragma once\n\n#include <cstdint>\n\nnamespace smb3 {\n")
        for name in sorted(syms):
            if name in RESERVED or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
                continue
            value = syms[name]
            if not isinstance(value, int) or not 0 <= value <= 0xFFFF:
                continue
            fh.write("constexpr uint16_t %s = 0x%04X;\n" % (name, value))
        fh.write("\n// ROM tables (byte offsets into prg.bin; the runtime maps an 8 KB bank\n")
        fh.write("// into any of $8000/$A000/$C000/$E000, so a runtime address's low 13 bits\n")
        fh.write("// are its offset within the bank file).\n")
        for name in ROM_LABELS:
            bank = LABEL_BANKS[name]
            fh.write("constexpr uint32_t %s_ROM = 0x%05X;  // bank %d, $%04X\n" %
                     (name, bank * 0x2000 + (syms[name] & 0x1FFF), bank, syms[name]))
        fh.write("}  // namespace smb3\n")
    # Dispatch table: bank index -> function pointer
    with open(os.path.join(args.out, "bank_table.inc"), "w") as fh:
        fh.write("// Generated. Index = PRG bank number (8KB). nullptr = no code.\n")
        fh.write("#define SMB3_BANK_COUNT 32\n")
        for b in range(32):
            if any(t[0] == b for t in table):
                fh.write("extern uint16_t bank_%02d(Machine&, uint16_t);\n" % b)
        fh.write("static BankFn g_bank_fns[SMB3_BANK_COUNT] = {\n")
        for b in range(32):
            name = "bank_%02d" % b if any(t[0] == b for t in table) else "nullptr"
            fh.write("  %s,\n" % name)
        fh.write("};\n")
    print("emitted %d instructions across %d banks into %s" % (total, len(table), args.out))
    print("sprite Y records: %d culling branches record %d skipped stores"
          % (SPR_SITES[0], SPR_SITES[1]))


if __name__ == "__main__":
    main()
