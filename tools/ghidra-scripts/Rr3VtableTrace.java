// Follow a global service pointer -> vtable -> slot -> function, then decompile it.
//
// Usage:
//   analyzeHeadless <projDir> <projName> -process libRealRacing3.so -noanalysis \
//       -scriptPath <dir> -postScript Rr3VtableTrace.java <outFile> <slotHex> [<slotHex> ...]
//
// Where <slotHex> is an ELF virtual address of a global pointer (the script adds the
// image base itself). For each one it prints the pointer value, then the object's
// vtable, then every slot's target, decompiling the requested slot index.
//
// Written for Ghidra 12.x headless. Java, because Jython is gone since 11.x.

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class Rr3VtableTrace extends GhidraScript {

    private DecompInterface decomp;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("ERROR: need <outFile> <globalPtrAddr> [<slotIndex> ...]");
            return;
        }
        File out = new File(args[0]);
        out.getParentFile().mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Address base = currentProgram.getImageBase();
        Memory mem = currentProgram.getMemory();

        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("program  : " + currentProgram.getName());
            w.println("imageBase: " + base);

            for (int i = 1; i < args.length; i++) {
                String raw = args[i];
                if (raw.startsWith("slot")) {
                    continue;
                }
                long vaddr = raw.startsWith("0x") ? Long.parseUnsignedLong(raw.substring(2), 16)
                                                  : Long.parseLong(raw);
                Address g = base.add(vaddr);
                w.println();
                w.println("================================================================");
                w.println("GLOBAL PTR " + raw + "  -> image addr " + g);

                Symbol sym = getSymbolAt(g);
                w.println("  symbol: " + (sym == null ? "<none>" : sym.getName(true)));
                w.println("  raw   : " + hex(readPtr(w, mem, g)));

                // NOTE: after relocation processing Ghidra stores .data pointers as
                // absolute IMAGE addresses already, so these dereferences must NOT
                // add the image base again -- doing so lands on the ELF header and
                // prints plausible-looking garbage.
                long objV = readPtr(w, mem, g);
                Address obj = toAddr(objV);
                w.println("  object: " + obj);
                Address vt = toAddr(readPtr(w, mem, obj));
                if (!mem.contains(vt)) {
                    w.println("  vtable: <unmapped>");
                    continue;
                }
                w.println("  vtable: " + vt);
                w.println("  --- slots ---");
                for (int s = 0; s <= 0x100; s += 8) {
                    Long t = readPtr(w, mem, vt.add(s));
                    String name = "";
                    if (t != null) {
                        Address ta = toAddr(t);
                        Function f = currentProgram.getFunctionManager().getFunctionAt(ta);
                        name = (f != null) ? "  " + f.getName()
                                           : "  (no function defined at " + ta + ")";
                    }
                    w.printf("    +0x%02x  slot %3d -> %s%s%n", s, s / 8,
                             t == null ? "<unreadable>" : String.format("0x%016x", t), name);
                }
            }
        }
        decomp.dispose();
        println("wrote " + out.getAbsolutePath());
    }

    private long readPtr(PrintWriter w, Memory mem, Address a) {
        try {
            byte[] b = new byte[8];
            mem.getBytes(a, b);
            long v = 0;
            for (int i = 7; i >= 0; i--) {
                v = (v << 8) | (b[i] & 0xffL);
            }
            return v;
        } catch (Exception e) {
            w.println("    <read failed at " + a + ": " + e.getMessage() + ">");
            return 0;
        }
    }

    private String hex(long v) {
        return String.format("0x%016x", v);
    }
}
