"""Fix the -Wformat-truncation= warnings in one C++ file, mechanically.

WHY: the port formats GUEST-supplied strings (paths, names) into fixed
buffers, and the compiler says the result may be truncated.  Twenty such sites
are in guest_filesystem.cpp.  Hand-editing twenty format strings is where
mistakes happen, so this does it from the compiler's own report.

HOW: for every warning the build log gives the source line, the destination
size and the source size.  The fix is to give every %s in that call a
precision of (size - 1) / (the number of %s) - 1, so the total can never
exceed the destination and the compiler can PROVE it.  The precision is
generous: the guest's paths are tens to hundreds of characters, so nothing
that used to fit is cut.

Usage: python fix-truncation.py <build.log> <source.cpp> [source.cpp ...]
"""
import re
import sys


def warnings_from(log_path: str):
    pattern = re.compile(r"^(.*?):(\d+):(\d+): warning: .*-Wformat-truncation=")
    out = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.match(line.strip())
            if match:
                out.append((match.group(1), int(match.group(2))))
    return out


def split_args(text: str, start: int):
    """Split the argument list of a call whose '(' is at text[start]."""
    depth = 0
    args = []
    current = []
    i = start
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            current.append(ch)
            if ch == "\\" and i + 1 < len(text):
                current.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            current.append(ch)
            i += 1
            continue
        if ch == "(":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(current))
                return args, i
        elif ch == "," and depth == 1:
            args.append("".join(current))
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    return args, -1


def buffer_size(text: str, name: str):
    match = re.search(r"(?:char|uint8_t|unsigned char)\s+" + re.escape(name) + r"\[([^\]]+)\]", text)
    if not match:
        return None
    size = match.group(1).strip()
    try:
        return int(size, 0)
    except ValueError:
        return None


def fix_file(path: str, lines_wanted) -> int:
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        text = handle.read()
    fixed = 0
    for lineno in sorted(set(lines_wanted), reverse=True):
        # The call may start a few lines above the reported line.
        window_start = text.rfind("snprintf", 0, self_offset(text, lineno))
        if window_start < 0:
            continue
        paren = text.find("(", window_start)
        if paren < 0:
            continue
        args, _ = split_args(text, paren)
        if len(args) < 3:
            continue
        size_match = re.search(r"sizeof\(\s*(\w+)\s*\)", args[1])
        if not size_match:
            continue
        size = buffer_size(text, size_match.group(1))
        if not size:
            continue
        format_arg = args[2]
        count = format_arg.count("%s")
        if count == 0:
            continue
        precision = max(1, (size - 1) // count - 1)
        new_format = format_arg.replace("%s", "%." + str(precision) + "s")
        if new_format == format_arg:
            continue
        # Rewrite the format argument in place.
        start = text.index(format_arg, paren)
        text = text[:start] + new_format + text[start + len(format_arg):]
        fixed += 1
    if fixed:
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as handle:
            handle.write(text)
    return fixed


def self_offset(text: str, lineno: int) -> int:
    offset = 0
    for _ in range(lineno - 1):
        nl = text.find("\n", offset)
        if nl < 0:
            return len(text)
        offset = nl + 1
    return offset


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    log_path = sys.argv[1]
    wanted = warnings_from(log_path)
    by_file = {}
    for path, lineno in wanted:
        by_file.setdefault(path, []).append(lineno)
    total = 0
    for path, linenos in by_file.items():
        count = fix_file(path, linenos)
        print("%s: %d call(s) fixed" % (path, count))
        total += count
    print("total: %d" % total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
