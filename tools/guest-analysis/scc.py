"""Tarjan SCC over a single function, treating `ret` as a terminator with no
successors and tail-call `b` as a jump (not fallthrough).

Why this exists: back_edges() counts three different things as "loops" on this
binary -- branches to an epilogue, shared continuation points, and
switch-table jumps. None of those is necessarily a cycle, and all three were
misread as loops during one investigation. A strongly connected component of
size > 1 is the only thing that is actually a loop, and a self-edge (a branch
whose target is itself) counts too.
"""
import sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import guestfde as G


def scc_report(va, label="", show_edges=4):
    r = G._range(va)
    if not r:
        return None
    start, end = r
    ins = {i.address: i for i in G.insns(va)}
    order = [a for a in sorted(ins)]

    def succ(a):
        i = ins[a]
        m, o = i.mnemonic, i.op_str
        if m == "ret":
            return []
        if m == "b" and o.startswith("#"):
            t = int(o.split(",")[-1][1:], 16)
            # a tail-call b can target another function; that is a control
            # transfer out of this one, not an edge inside it
            return [t] if t in ins else []
        if m in ("b.eq", "b.ne", "b.lt", "b.le", "b.gt", "b.ge", "b.hi",
                 "b.hs", "b.lo", "b.ls", "b.mi", "b.pl", "b.vs", "b.vc",
                 "b.mi", "cbz", "cbnz", "tbz", "tbnz"):
            out = []
            nxt = a + 4
            if nxt in ins:
                out.append(nxt)
            if "#" in o:
                try:
                    t = int(o.split("#")[1].split(",")[0], 16)
                    if t in ins:
                        out.append(t)
                except ValueError:
                    pass
            return out
        nxt = a + 4
        return [nxt] if nxt in ins else []

    index = {}
    low = {}
    onstack = {}
    stack = []
    counter = [0]
    comps = []

    def strong(v):
        work = [(v, 0)]
        while work:
            node, pi = work[-1]
            if pi == 0:
                index[node] = low[node] = counter[0]
                counter[0] += 1
                stack.append(node)
                onstack[node] = True
            recurse = False
            ss = succ(node)
            for k in range(pi, len(ss)):
                w = ss[k]
                if w not in index:
                    work[-1] = (node, k + 1)
                    work.append((w, 0))
                    recurse = True
                    break
                elif onstack.get(w):
                    low[node] = min(low[node], index[w])
            if recurse:
                continue
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    onstack[w] = False
                    comp.append(w)
                    if w == node:
                        break
                comps.append(comp)
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])

    for a in order:
        if a not in index:
            strong(a)

    real = [c for c in comps if len(c) > 1]
    # a self-loop is a 1-node SCC whose node branches to itself
    for a in order:
        if a in succ(a) and all(a not in c for c in comps):
            real.append([a])

    size = end - start
    print("=== %s0x%09x  [%0x,0x%x)  %d bytes, %d insns ==="
          % (label + " " if label else "", va, start, end, size, len(ins)))
    print("  SCCs total=%d   REAL CYCLES=%d" % (len(comps), len(real)))
    for c in sorted(real, key=len, reverse=True)[:6]:
        lo, hi = min(c), max(c)
        print("    cycle: %d insns  [0x%x .. 0x%x]  span=%d B"
              % (len(c), lo, hi, hi - lo))
        for a in order:
            if lo <= a <= hi and a in succ(a):
                print("       self-loop at 0x%09x: %s %s" % (a, ins[a].mnemonic, ins[a].op_str))
        shown = 0
        for a in order:
            if lo <= a <= hi and shown < show_edges:
                for t in succ(a):
                    if lo <= t <= hi and t <= a:
                        i = ins[a]
                        print("       back edge 0x%09x %-7s -> 0x%09x   %s %s"
                              % (a, i.mnemonic, t, i.mnemonic, i.op_str))
                        shown += 1
                        break
    print()
    return real


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        va, _, lbl = arg.partition(":")
        scc_report(int(va, 16), lbl)
