// List every reference to a global, split into writes vs reads, and decompile the
// functions that contain the WRITES (those are the registration / construction sites).
//
// Usage:
//   analyzeHeadless <projDir> <projName> -process <prog> -noanalysis \
//       -scriptPath <dir> -postScript Rr3RefScan.java <outFile> <elfVaddr> [...]
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
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class Rr3RefScan extends GhidraScript {

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

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("program=" + currentProgram.getName() + " imageBase=" + base);
            for (int i = 1; i < args.length; i++) {
                String raw = args[i];
                long v = raw.startsWith("0x") ? Long.parseUnsignedLong(raw.substring(2), 16)
                                             : Long.parseLong(raw);
                Address g = base.add(v);
                w.println();
                w.println("================================================================");
                w.println("REFERENCES TO " + raw + "  -> " + g);

                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(g);
                Set<Address> writeSites = new TreeSet<>();
                Set<Address> readSites = new TreeSet<>();
                int n = 0;
                while (it.hasNext()) {
                    Reference r = it.next();
                    n++;
                    if (r.getReferenceType().isWrite()) {
                        writeSites.add(r.getFromAddress());
                    } else {
                        readSites.add(r.getFromAddress());
                    }
                }
                w.println("  total references : " + n);
                w.println("  write sites      : " + writeSites.size());
                w.println("  read  sites      : " + readSites.size());
                for (Address a : writeSites) {
                    Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
                    w.println("    W  " + a + "  in " + (f == null ? "<no function>" : f.getName()
                            + " @" + f.getEntryPoint()));
                }
                if (readSites.size() <= 30) {
                    for (Address a : readSites) {
                        Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
                        w.println("    r  " + a + "  in " + (f == null ? "<no function>" : f.getName()));
                    }
                } else {
                    w.println("    (read sites suppressed, " + readSites.size() + " of them)");
                }

                // Decompile every function that contains a write.
                Set<Function> fns = new LinkedHashSet<>();
                for (Address a : writeSites) {
                    Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
                    if (f != null) {
                        fns.add(f);
                    }
                }
                w.println("  --- decompile of " + fns.size() + " writing function(s) ---");
                for (Function f : fns) {
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
}
