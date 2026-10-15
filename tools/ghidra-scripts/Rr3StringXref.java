// Find every reference to a string/data address and decompile the functions that read it.
//
// Usage:
//   analyzeHeadless <projDir> <projName> -process <prog> -noanalysis \
//       -scriptPath <dir> -postScript Rr3StringXref.java <outFile> <elfVaddr> [...]
//
// Written for Ghidra 12.x headless. Java, because Jython is gone since 11.x.

import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class Rr3StringXref extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("ERROR: need <outFile> <elfVaddr> [...]");
            return;
        }
        File out = new File(args[0]);
        out.getParentFile().mkdirs();
        Address base = currentProgram.getImageBase();
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("program=" + currentProgram.getName() + " imageBase=" + base);
            for (int i = 1; i < args.length; i++) {
                String raw = args[i];
                long v = raw.startsWith("0x") ? Long.parseUnsignedLong(raw.substring(2), 16)
                                             : Long.parseLong(raw);
                Address a = base.add(v);
                w.println();
                w.println("================================================================");
                w.println("XREFS TO " + raw + "  -> " + a);
                w.println("  string at target: " + readCString(w, mem, a));

                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                Set<Address> sites = new TreeSet<>();
                while (it.hasNext()) {
                    Reference r = it.next();
                    sites.add(r.getFromAddress());
                }
                w.println("  reference sites: " + sites.size());

                Set<Function> fns = new LinkedHashSet<>();
                for (Address s : sites) {
                    Function f = currentProgram.getFunctionManager().getFunctionContaining(s);
                    String tag = (f == null) ? "<no function>" : f.getName() + " @" + f.getEntryPoint();
                    w.println("    " + s + "  in " + tag);
                    if (f != null) {
                        fns.add(f);
                    }
                }

                w.println("  --- decompile of " + fns.size() + " referencing function(s) ---");
                int done = 0;
                for (Function f : fns) {
                    if (done >= 8) {
                        w.println("  (capped at 8)");
                        break;
                    }
                    done++;
                    w.println();
                    w.println("---- " + f.getName() + " @" + f.getEntryPoint() + " ----");
                    DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                    if (r == null || !r.decompileCompleted()) {
                        w.println("  <decompile failed>");
                        continue;
                    }
                    w.println(r.getDecompiledFunction().getC());
                }
            }
        }
        decomp.dispose();
        println("wrote " + out.getAbsolutePath());
    }

    private String readCString(PrintWriter w, Memory mem, Address a) {
        try {
            byte[] buf = new byte[160];
            int n = 0;
            while (n < buf.length) {
                byte b = mem.getByte(a.add(n));
                if (b == 0) {
                    break;
                }
                buf[n++] = b;
            }
            return new String(buf, 0, n, "UTF-8");
        } catch (Exception e) {
            return "<unreadable: " + e.getMessage() + ">";
        }
    }
}