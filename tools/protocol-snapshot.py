#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 rc4l
#
# protocol-snapshot.py -- pin the client/server WIRE FORMAT so a backport can't
# silently change it.
#
# Every SERVERCOMMANDS_* command is generated from src/zandronum/protocolspec/*.txt.
# A change there -- a field added, reordered, retyped, made (un)conditional, or a
# command inserted (which shifts every later command's wire ID) -- changes what
# goes on the network. Offline builds and single-player never notice; multiplayer
# silently desyncs. This tool derives a canonical signature of the whole protocol
# by REUSING THE GENERATOR'S OWN PARSER (so the snapshot cannot drift from the code
# the engine actually runs) and diffs it against a committed golden.
#
#   tools/protocol-snapshot.py --write   # regenerate the golden (deliberate change)
#   tools/protocol-snapshot.py --check    # CI: fail if the wire format moved
#
# The signature is TYPE-based, not name-based: renaming a field is not a wire
# change and must not trip the check; changing its type, order, condition, or the
# command's ID is. Each line is:
#
#   <id> <SVC|SVC2>_<NAME> [flags] : <type>[<spec>][?] <type>[<spec>][?] ...
#
# where a trailing '?' marks a conditional field and [flags] notes Unreliable /
# Extended framing -- both wire-visible.
#
# [Kaori-Chani] Two things the signature used to leave out, each a width change it would have
# slept through:
#
#   - A compound field is expanded, so a change inside it moves this file. A struct prints as
#     Struct<Name>{member member ...} and an array as Array<Byte>{element}, the Byte being the
#     count the generator writes ahead of the elements. SVC2_SNAPSPECIALSTATE read
#     "Short Struct Short Struct" before, which said nothing about either struct's members.
#   - A hand-framed command gets a line of its own, in a "## hand-framed commands" section.
#     Those are built with NetCommand in sv_commands.cpp and the snapshot module rather than
#     generated from the spec, so the spec cannot describe them and SVC_DOPUSHER, SVC2_DOORSOUND
#     and a hundred others carried no field list here at all. Their fields are what the sender
#     itself writes, in order, with '?' for a field inside a conditional and '*' for one inside a
#     loop; a field a called function writes is not seen, and one id can hold several shapes (as
#     SVC2_UPDATEMAPROTATION's sub-commands do), each on its own line.
#
# [MobiusU9B] <id> is the command's position in the spec files, but the byte on the wire is its value in network_enums.h, so a trailing "## wire ids" section pins those values too.
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "src/zandronum/protocolspec/spec.txt")
GOLDEN = os.path.join(ROOT, "tools/data/protocol-snapshot.txt")
GEN = os.path.join(ROOT, "src/zandronum/protocolspec/generator")
ENUMS = os.path.join(ROOT, "src/zandronum/src/network_enums.h")

# [MobiusU9B] The network_enums.h enums whose values are the command bytes sent on the wire.
WIRE_ENUMS = ("ServerConnectionCommand", "SVC", "SVC2", "CLCC", "CLC")

HEADER = (
    "# AUTO-GENERATED wire-format snapshot -- do not hand-edit.\n"
    "# Regenerate with: tools/protocol-snapshot.py --write\n"
    "# One line per network command: <spec-order> <enum> [flags] : <field-types-in-order>,\n"
    "# a struct expanded as Struct<Name>{members} and an array as Array<Byte>{element};\n"
    "# then the hand-framed commands, whose fields their own senders write rather than the spec,\n"
    "# with ? for a conditional field and * for one written in a loop;\n"
    "# then one line per wire command id: <value> <enum>, as network_enums.h assigns it.\n"
    "# A diff here means a backport changed the client/server wire format. Confirm it\n"
    "# is intentional AND that older clients can still parse it before updating this file.\n"
)


def load_spec():
    sys.path.insert(0, GEN)
    from spec import NetworkSpec  # the generator's own parser -- single source of truth
    spec = NetworkSpec()
    spec.loadfromfile(SPEC)
    return spec


def type_sig(param, seen=()):
    """[Kaori-Chani] The wire shape of one field's type. A struct is expanded into its members
    and an array into the Byte count the generator writes ahead of its elements, so a change
    inside either shows here rather than hiding behind the word Struct."""
    import parametertypes
    if isinstance(param, parametertypes.ArrayParameter):
        return "Array<Byte>{%s}" % type_sig(param.elementType, seen)
    if isinstance(param, parametertypes.StructParameter):
        name = param.specialization
        # A struct holding itself has no wire shape to print, and the generator would not
        # terminate on one either.
        if name in seen:
            return "Struct<%s>{...}" % name
        members = param.struct["members"].values()
        return "Struct<%s>{%s}" % (name,
            " ".join(field_sig(m, seen + (name,)) for m in members))
    sig = param.typename
    if getattr(param, "specialization", None):
        sig += "<%s>" % param.specialization
    elif getattr(param, "indexLength", None):
        # [Kaori-Chani] A Sector, Line or Side travels as an index of its own width, so
        # the width is the wire shape: without it a Sector read as a Short and one read
        # as a Long printed the same word and a widening moved nothing here.
        sig += "<%s>" % param.indexLength
    return sig


def field_sig(param, seen=()):
    """Wire signature of one field: type, specialization, and the condition that
    gates it. No field NAME -- a rename is not a wire change and must not fire."""
    sig = type_sig(param, seen)
    cond = getattr(param, "condition", None)
    if cond:
        # Which flag/bit gates this optional field -- changing it changes the wire.
        sig += "?{%s}" % " ".join(str(cond).split())
    return sig


# [Kaori-Chani] The hand-framed senders: commands built with NetCommand rather than generated
# from the spec, so the spec cannot describe their fields and a width change in one of them moved
# nothing here. servercommands.cpp is left out: its NetCommands are the generated ones.
HAND_SOURCES = (
    "src/zandronum/src/sv_commands.cpp",
    "src/zandronum/src/features/net-snapshot/capture.cpp",
)

# NetCommand's writers (network/netcommand.h), each one a field on the wire.
HAND_ADDERS = {
    "addByte": "Byte", "addShort": "Short", "addLong": "Long", "addFloat": "Float",
    "addString": "String", "addName": "Name", "addBit": "Bit", "addVariable": "Variable",
    "addBuffer": "Buffer",
}
# The two that take their width as an argument, which is itself wire-visible.
HAND_SIZED = {"addShortByte": "ShortByte", "addInteger": "Integer"}

COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def after_condition(line):
    """[Kaori-Chani] What a control keyword's line holds after its own parentheses: "{" when the
    block opens here, "" when the body is on the next line, and a statement when it is here."""
    start = line.find("(")
    if start < 0:
        return line[line.find(" ") + 1:].strip() if " " in line else ""
    depth = 0
    for i in range(start, len(line)):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
            if depth == 0:
                return line[i + 1:].strip()
    return ""


def hand_blocks(text):
    """[Kaori-Chani] One entry per NetCommand built in `text`: its id, its fields in order and
    its framing flags. A field written inside a loop is marked *, one inside any other nested
    block ?, since neither goes out unconditionally. A field a called function writes is not
    seen, which is why the header says these lines are what the sender itself writes."""
    out = []
    cur = None
    stack = []          # one entry per open brace: "loop", "cond" or "plain"
    pending = None      # what the next opening brace stands for
    single = None       # the same for a body written without braces, good for one statement
    for raw in COMMENTS.sub("", text).splitlines():
        line = raw.strip()
        held = single
        single = None
        opener = None
        if re.match(r"^(for|while|do)\b", line):
            opener = "loop"
        elif re.match(r"^(if|else|switch|case)\b", line):
            opener = "cond"
        if opener is not None:
            # What follows the keyword's own parentheses decides where its body is: a brace here
            # or on the next line, a statement here, or a statement on the next line with no
            # braces at all -- which is as conditional as a block and is how SVC2_SNAPSOUND's
            # setUnreliable is written.
            tail = after_condition(line)
            if tail.startswith("{") or (tail == ""):
                pending = opener
                single = opener
            else:
                held = opener
        decl = re.search(r"NetCommand\s+(\w+)\s*\(\s*(SVC2?_\w+)", line)
        if decl is not None:
            if cur is not None:
                out.append(cur)
            cur = {"var": decl.group(1), "id": decl.group(2), "fields": [], "flags": [],
                   "depth": len(stack)}
        if cur is not None:
            inner = stack[cur["depth"]:] + ([held] if held else [])
            mark = "*" if ("loop" in inner) else ("?" if inner else "")
            for call in re.finditer(r"\b%s\.(\w+)\s*\(([^;]*)" % re.escape(cur["var"]), line):
                name, args = call.group(1), call.group(2)
                if name in HAND_ADDERS:
                    cur["fields"].append(HAND_ADDERS[name] + mark)
                elif name in HAND_SIZED:
                    width = args.rsplit(",", 1)[-1].strip().rstrip(")").strip() if "," in args \
                        else "?"
                    cur["fields"].append("%s<%s>%s" % (HAND_SIZED[name], width, mark))
                elif name == "setUnreliable":
                    cur["flags"].append("Unreliable" + ("?" if mark else ""))
        # The braces this line opens and closes, after its calls have been read.
        for ch in line:
            if ch == "{":
                stack.append(pending or "plain")
                pending = None
            elif ch == "}":
                if stack:
                    stack.pop()
                if (cur is not None) and (len(stack) < cur["depth"]):
                    out.append(cur)
                    cur = None
        # A plain statement ends whatever a keyword line was holding open; a keyword line's own
        # semicolons (a for header's) must not, or its block reads as an ordinary scope.
        if (opener is None) and ("{" not in line) and (";" in line):
            pending = None
    if cur is not None:
        out.append(cur)
    return out


def handwritten():
    """[Kaori-Chani] The hand-framed commands' lines, one per id and shape, sorted by id so a
    sender moving within its file is not a diff while its fields moving is."""
    seen = {}
    for rel in HAND_SOURCES:
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            for block in hand_blocks(f.read()):
                flags = (" [%s]" % ",".join(block["flags"])) if block["flags"] else ""
                fields = " ".join(block["fields"]) or "(no fields)"
                seen.setdefault((block["id"], flags, fields), None)
    lines = ["## hand-framed commands (%s)" % ", ".join(HAND_SOURCES)]
    for cid, flags, fields in sorted(seen):
        lines.append("%-38s%s : %s" % (cid, flags, fields))
    return lines


def snapshot():
    spec = load_spec()
    lines = []
    # Commands live under each protocol; iterate protocols in definition order, and
    # within each, number base (SVC) and extended (SVC2) commands separately by parse
    # order, so a command inserted mid-spec shifts every later line.
    # [MobiusU9B] Those numbers are not the wire ids, which come from network_enums.h and are pinned by wire_ids() below.
    for proto_name, proto in spec.protocols.items():
        lines.append("## protocol %s" % proto_name)
        counters = {False: 0, True: 0}
        for cmd in proto["commands"].values():
            cid = counters[cmd.extended]
            counters[cmd.extended] += 1
            flags = []
            if cmd.extended:
                flags.append("Extended")
            if getattr(cmd, "unreliable", False):
                flags.append("Unreliable")
            flagstr = (" [%s]" % ",".join(flags)) if flags else ""
            fields = " ".join(field_sig(p) for p in cmd) or "(no fields)"
            lines.append("%3d %-38s%s : %s" % (cid, cmd.enumname, flagstr, fields))
    lines.extend(handwritten())
    lines.extend(wire_ids())
    return HEADER + "\n".join(lines) + "\n"


def wire_ids():
    """[MobiusU9B] The value of every element of the wire enums, resolved the way the compiler does."""
    with open(ENUMS) as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    values = {}
    blocks = {}
    for name, body in re.findall(r"BEGIN_ENUM\s*\(\s*(\w+)\s*\)(.*?)END_ENUM", text, flags=re.S):
        nextval = 0
        elements = []
        for elem, init in re.findall(r"ENUM_ELEMENT2?\s*\(\s*(\w+)\s*(?:,\s*(\w+)\s*)?\)", body):
            if init:
                nextval = values[init] if init in values else int(init, 0)
            values[elem] = nextval
            elements.append((nextval, elem))
            nextval += 1
        blocks[name] = elements
    lines = ["## wire ids (network_enums.h)"]
    for name in WIRE_ENUMS:
        lines.append("# %s" % name)
        lines.extend("%3d %s" % (value, elem) for value, elem in blocks[name])
    return lines


def command_count(snap):
    """[MobiusU9B] Number of command lines, the only lines carrying a field list."""
    return sum(1 for line in snap.splitlines() if " : " in line)


def main():
    mode =sys.argv[1] if len(sys.argv) > 1 else "--check"
    current = snapshot()
    if mode == "--write":
        os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
        with open(GOLDEN, "w") as f:
            f.write(current)
        print("protocol-snapshot: wrote golden (%d commands)" % command_count(current))
        return 0
    # --check
    if not os.path.exists(GOLDEN):
        print("protocol-snapshot: no golden at %s -- run --write once" % GOLDEN, file=sys.stderr)
        return 1
    with open(GOLDEN) as f:
        golden = f.read()
    if current == golden:
        print("protocol-snapshot: wire format unchanged (%d commands)." % command_count(current))
        return 0
    import difflib
    print("protocol-snapshot: THE WIRE FORMAT CHANGED. A backport altered the client/server "
          "protocol.\nConfirm it is intentional and older clients still parse it, then run "
          "`tools/protocol-snapshot.py --write` to accept it.\n", file=sys.stderr)
    for line in difflib.unified_diff(golden.splitlines(True), current.splitlines(True),
                                     "golden", "current"):
        sys.stderr.write(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
