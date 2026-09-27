"""Hold the INI Master metadata up against the code that reads the ini.

`mod/data/FlightFreedom.inimeta` is compiled into the plugin as the
INIMETA resource, and INI Master shows people what it says: the type of
each key, its default and its help. None of that is read from the code.
A new key, a changed default, a retired setting: each has to be copied
across by hand, and a wrong copy does not fail anywhere on its own. This
is the check that it was copied.

The code is the authority. `ReadSettings()` in `mod/src/core/mod.cpp` is
the only place `FlightFreedom.ini` is read, one `ReadSetting(L"Key",
L"default")` call per key, some of them followed by `!= 0.0f` to turn the
float into a bool. Every key `ReadSettings()` reads has to be in the
metadata's `[settings]` section, and every key described there has to be
one `ReadSettings()` reads. The type follows the reader: a call with
`!= 0.0f` is bool, a bare call is float. A bare call may still be
described as bool when `Worker()` never uses the field as a number
either, only ever testing it against zero; `BlackstarStays` is why that
exception exists, stored as a float and read with a bare call, tested
nowhere but `!= 0.0f` and `!= 1.0f`, so the ini's own on/off switch is the
honest description. The default in the metadata has to match the
fallback string literal in that same `ReadSetting()` call.

`[sites]`, `[patch]` and `[watch]` hold keys a player types in by hand,
read by a different parser in `sites.cpp`, not a fixed settings list.
This script leaves their contents unchecked and only confirms the
metadata still carries all three sections, each marked hidden, so a
careless edit cannot drop one silently.

Exits non-zero on any mismatch, so it can gate a release.

    py -3 scripts/check_inimeta.py
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
META = os.path.join(ROOT, "mod", "data", "FlightFreedom.inimeta")
MOD_CPP = os.path.join(ROOT, "mod", "src", "core", "mod.cpp")


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def body(src, head):
    """The brace-balanced body of the first function whose signature starts with head."""
    i = src.index(head)
    i = src.index("{", i)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise ValueError(head)


def parse_reader(cpp):
    """key -> (member, kind, default) from ReadSettings()."""
    src = body(cpp, "Settings ReadSettings()")
    out = {}
    pat = re.compile(
        r's\.(\w+)\s*=\s*ReadSetting\(L"(\w+)",\s*L"([^"]*)"\)(\s*!=\s*0\.0f)?\s*;')
    for m in pat.finditer(src):
        member, key, default, isbool = m.group(1), m.group(2), m.group(3), m.group(4)
        out[key] = {"member": member, "kind": "bool" if isbool else "float", "default": default}
    return out


def strip_calls(src, names):
    """Remove every balanced call to one of `names` (e.g. LOG, LOG_ERR), so a
    value that only appears inside a diagnostic message does not count as a
    use of that value. A cast for display is not a use."""
    out = []
    i = 0
    pat = re.compile(r'\b(?:%s)\s*\(' % "|".join(re.escape(n) for n in names))
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        out.append(src[i:m.start()])
        depth = 1
        j = m.end()
        while j < len(src) and depth:
            if src[j] == "(":
                depth += 1
            elif src[j] == ")":
                depth -= 1
            j += 1
        i = j
    return "".join(out)


def bool_only_members(cpp):
    """Members ReadSettings() reads as a bare float, but that the rest of the
    file only ever compares against 0.0f (or 1.0f) -- never uses as a number.
    Worker() is read whole rather than isolated, since the comparisons for a
    given field are not necessarily contiguous. Log lines are stripped first:
    a field cast for display in a LOG() call is not a use of its value."""
    reader = parse_reader(cpp)
    bare_members = {v["member"] for v in reader.values() if v["kind"] == "float"}
    scan = strip_calls(cpp, ("LOG", "LOG_ERR", "LOG_OK"))
    result = set()
    for member in bare_members:
        uses = re.findall(r's\.%s\b\s*([^;,)\n]*)' % re.escape(member), scan)
        # Drop the ReadSettings() assignment itself (starts with "=").
        uses = [u for u in uses if not u.strip().startswith("=")]
        if uses and all(re.match(r'(!=|==)\s*[01]\.0f', u.strip()) for u in uses):
            result.add(member)
    return result


def load_meta():
    text = read(META)
    text = re.sub(r"^\s*//.*\n", "", text, flags=re.M)
    return json.loads(text)


def main():
    cpp = read(MOD_CPP)
    reader = parse_reader(cpp)
    boolish = bool_only_members(cpp)
    meta = load_meta()
    sections = meta.get("sections", {})
    keys = sections.get("settings", {}).get("keys", {})
    errors = []

    for k in sorted(set(reader) - set(keys)):
        errors.append("%s: ReadSettings() reads it and the metadata does not describe it" % k)
    for k in sorted(set(keys) - set(reader)):
        errors.append("%s: in the metadata but ReadSettings() never reads it" % k)

    for k, spec in keys.items():
        if k not in reader:
            continue
        r = reader[k]
        want_types = {r["kind"]}
        if r["kind"] == "float" and r["member"] in boolish:
            want_types.add("bool")
        t = spec.get("type")
        if t not in want_types:
            errors.append("%s: type %s in the metadata, ReadSettings() reads it as %s" % (k, t, r["kind"]))

        got, want = spec.get("default"), r["default"]
        if got is None:
            errors.append("%s: no default in the metadata" % k)
        else:
            try:
                match = abs(float(got) - float(want)) < 1e-6
            except ValueError:
                match = str(got) == str(want)
            if not match:
                errors.append("%s: default %s in the metadata, ReadSetting()'s fallback is %s" % (k, got, want))

    for name in ("sites", "patch", "watch"):
        sec = sections.get(name)
        if sec is None:
            errors.append("%s: no [%s] section in the metadata, and sites.cpp reads that section" % (name, name))
        elif not sec.get("hidden"):
            errors.append("%s: [%s] holds research keys the ini's own comments say are read-only with "
                           "Probe=1, and the metadata does not mark it hidden" % (name, name))

    for line in errors:
        print("error  " + line)
    print("%d keys in the metadata, %d read by ReadSettings(), %d errors"
          % (len(keys), len(reader), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
