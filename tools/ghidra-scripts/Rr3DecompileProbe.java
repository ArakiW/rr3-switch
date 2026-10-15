// Headless decompiler probe for libRealRacing3.so.
//
// Usage:
//   analyzeHeadless <projDir> <projName> -process <prog> -noanalysis \
//       -scriptPath <dir> -postScript Rr3DecompileProbe.java <outFile> <vaddr> [<vaddr> ...]
//
// For each requested ELF virtual address it:
//   1. resolves the containing function (ET_DYN -> image base + vaddr),
//   2. decompiles it and writes the C to <outFile>,
//   3. decompiles each direct caller (one level), and
//   4. prints a summary table: function name, entry, size, caller count.
//
// Written for Ghidra 12.x. Uses the Java scripting API because the Jython
// runtime was removed in Ghidra 11; PyGhidra is the interactive alternative but
// is not needed for a headless pass.

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class Rr3DecompileProbe extends GhidraScript {

    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("ERROR: need <outFile> <vaddr> [<vaddr> ...]");
            return;
        }
        File out = new File(args[0]);
        out.getParentFile().mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        Address base = currentProgram.getImageBase();
        println("program   : " + currentProgram.getName());
        println("imageBase : " + base);
        println("functions : " + currentProgram.getFunctionManager().getFunctionCount());

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("### program=" + currentProgram.getName()
                    + " imageBase=" + base + " args=" + args.length);
            for (int i = 1; i < args.length; i++) {
                String raw = args[i];
                long vaddr = raw.startsWith("0x") ? Long.parseUnsignedLong(raw.substring(2), 16)
                                                  : Long.parseLong(raw);
                probe(w, base, vaddr, raw);
            }
        }
        decomp.dispose();
        println("wrote " + out.getAbsolutePath());
    }

    private void probe(PrintWriter w, Address base, long vaddr, String label) {
        Address addr = base.add(vaddr);
        w.println();
        w.println("================================================================");
        w.println("REQUEST " + label + "  -> image address " + addr);

        Function f = currentProgram.getFunctionManager().getFunctionContaining(addr);
        if (f == null) {
            w.println("  NO FUNCTION CONTAINS THIS ADDRESS (not analysed / not code)");
            // fall back: is there code bytes at all?
            try {
                byte[] b = new byte[16];
                currentProgram.getMemory().getBytes(addr, b);
                StringBuilder sb = new StringBuilder();
                for (byte x : b) sb.append(String.format("%02x", x));
                w.println("  raw bytes: " + sb);
            } catch (Exception e) {
                w.println("  raw bytes: <unmapped: " + e.getMessage() + ">");
            }
            return;
        }

        w.println("  function : " + f.getName());
        w.println("  entry    : " + f.getEntryPoint() + "  body_end=" + f.getBody().getMaxAddress()
                + "  size=" + f.getBody().getNumAddresses());
        w.println("  signature: " + f.getSignature());

        Map<Function, Integer> callers = new LinkedHashMap<>();
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function c = currentProgram.getFunctionManager().getFunctionContaining(r.getFromAddress());
            if (c != null) {
                callers.merge(c, 1, Integer::sum);
            } else {
                w.println("  xref (no containing fn) from " + r.getFromAddress());
            }
        }
        w.println("  direct callers: " + callers.size());
        for (Map.Entry<Function, Integer> e : callers.entrySet()) {
            w.println("      " + e.getKey().getName() + " @" + e.getKey().getEntryPoint()
                    + " x" + e.getValue());
        }

        dump(w, "TARGET", f);

        // One level of callers, but only if there are not too many of them --
        // a 31 MB library can have hundreds and the output would be useless.
        int emitted = 0;
        for (Function c : callers.keySet()) {
            if (emitted >= 6) {
                w.println("  (caller decompilation capped at 6 of " + callers.size() + ")");
                break;
            }
            dump(w, "CALLER-OF-TARGET", c);
            emitted++;
        }

        // Callees of the target: often the next question is "what does it call".
        List<Function> callees = new ArrayList<>();
        for (Function c : f.getCalledFunctions(monitor)) {
            callees.add(c);
        }
        w.println("  direct callees: " + callees.size());
        for (int i = 0; i < Math.min(callees.size(), 40); i++) {
            Function c = callees.get(i);
            w.println("      " + c.getName() + " @" + c.getEntryPoint());
        }
    }

    private void dump(PrintWriter w, String tag, Function f) {
        w.println();
        w.println("---- " + tag + ": " + f.getName() + " @" + f.getEntryPoint() + " ----");
        DecompileResults r = decomp.decompileFunction(f, 120, monitor);
        if (r == null || !r.decompileCompleted()) {
            w.println("  <decompile failed: " + (r == null ? "null result" : r.getErrorMessage()) + ">");
            return;
        }
        w.println(r.getDecompiledFunction().getC());
    }
}
