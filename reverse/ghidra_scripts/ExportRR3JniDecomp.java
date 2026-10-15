// Export selected Real Racing 3 JNI functions as Ghidra C-like decompilation.
// @category RR3

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class ExportRR3JniDecomp extends GhidraScript {
    private static String safeFileName(String value) {
        return value.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("Usage: ExportRR3JniDecomp.java <output-dir>");
        }

        File outputDir = new File(args[0]);
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            throw new IllegalStateException("Cannot create output directory: " + outputDir);
        }

        List<Function> targets = new ArrayList<>();
        for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
            String name = function.getName();
            if (name.startsWith("Java_com_firemint_realracing_") ||
                name.startsWith("Java_com_firemonkeys_cloudcellapi_") ||
                name.equals("JNI_OnLoad")) {
                targets.add(function);
            }
        }
        targets.sort(Comparator.comparing(Function::getName));

        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Decompiler could not open the current program.");
        }

        File indexFile = new File(outputDir, "index.tsv");
        try (BufferedWriter index = new BufferedWriter(new FileWriter(indexFile))) {
            index.write("entry\tname\tstatus\tfile\n");
            for (Function function : targets) {
                if (monitor.isCancelled()) {
                    break;
                }
                String name = function.getName();
                String fileName = safeFileName(name) + ".c";
                DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                String status;
                String body;
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    status = "ok";
                    body = result.getDecompiledFunction().getC();
                }
                else {
                    status = "failed";
                    body = "/* " + result.getErrorMessage() + " */\n";
                }

                try (BufferedWriter writer = new BufferedWriter(
                        new FileWriter(new File(outputDir, fileName)))) {
                    writer.write("/* entry: " + function.getEntryPoint() + " */\n");
                    writer.write(body);
                }
                index.write(function.getEntryPoint() + "\t" + name + "\t" + status + "\t" + fileName + "\n");
                println(status + " " + function.getEntryPoint() + " " + name);
            }
        }
        decompiler.dispose();
        println("Exported " + targets.size() + " functions to " + outputDir.getAbsolutePath());
    }
}

