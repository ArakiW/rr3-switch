// Export a bounded call tree around the platform lifecycle and input JNI roots.
// @category RR3

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportRR3CallTree extends GhidraScript {
    private static final String[] ROOTS = {
        "Java_com_firemint_realracing_MainActivity_onCreateJNI",
        "Java_com_firemint_realracing_MainActivity_onStartJNI",
        "Java_com_firemint_realracing_MainActivity_onResumeJNI",
        "Java_com_firemint_realracing_MainActivity_onViewCreatedJNI",
        "Java_com_firemint_realracing_MainActivity_onViewChangedJNI",
        "Java_com_firemint_realracing_MainActivity_onViewRenderJNI",
        "Java_com_firemint_realracing_MainActivity_onKeyPressed",
        "Java_com_firemint_realracing_MainActivity_onKeyReleased",
        "Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI",
        "Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI",
        "Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI"
    };

    private static final class Node {
        final Function function;
        final int depth;
        final String root;

        Node(Function function, int depth, String root) {
            this.function = function;
            this.depth = depth;
            this.root = root;
        }
    }

    private static String safe(String value) {
        return value.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    private Function findFunction(String name) {
        SymbolIterator symbols = currentProgram.getSymbolTable().getSymbols(name);
        while (symbols.hasNext()) {
            Symbol symbol = symbols.next();
            Function function = currentProgram.getFunctionManager().getFunctionAt(symbol.getAddress());
            if (function != null) {
                return function;
            }
        }
        return null;
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("Usage: ExportRR3CallTree.java <output-dir> [max-depth]");
        }
        File outputDir = new File(args[0]);
        int maxDepth = args.length > 1 ? Integer.parseInt(args[1]) : 2;
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            throw new IllegalStateException("Cannot create output directory: " + outputDir);
        }

        ArrayDeque<Node> queue = new ArrayDeque<>();
        Set<String> visited = new HashSet<>();
        List<Node> nodes = new ArrayList<>();
        for (String rootName : ROOTS) {
            Function root = findFunction(rootName);
            if (root != null) {
                queue.add(new Node(root, 0, rootName));
            }
            else {
                printerr("Missing root: " + rootName);
            }
        }

        while (!queue.isEmpty() && nodes.size() < 1000 && !monitor.isCancelled()) {
            Node node = queue.removeFirst();
            String key = node.root + "|" + node.function.getEntryPoint();
            if (!visited.add(key)) {
                continue;
            }
            nodes.add(node);
            if (node.depth >= maxDepth) {
                continue;
            }
            for (Function called : node.function.getCalledFunctions(monitor)) {
                if (!called.isExternal()) {
                    queue.addLast(new Node(called, node.depth + 1, node.root));
                }
            }
        }
        nodes.sort(Comparator.comparing((Node n) -> n.root)
            .thenComparingInt(n -> n.depth)
            .thenComparing(n -> n.function.getEntryPoint()));

        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Decompiler could not open the current program.");
        }

        try (BufferedWriter index = new BufferedWriter(new FileWriter(new File(outputDir, "index.tsv")))) {
            index.write("root\tdepth\tentry\tname\tstatus\tfile\n");
            Set<String> exportedBodies = new HashSet<>();
            for (Node node : nodes) {
                Function function = node.function;
                String fileName = function.getEntryPoint() + "_" + safe(function.getName()) + ".c";
                String status = "shared";
                if (exportedBodies.add(function.getEntryPoint().toString())) {
                    DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                    String body;
                    if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                        status = "ok";
                        body = result.getDecompiledFunction().getC();
                    }
                    else {
                        status = "failed";
                        body = "/* " + result.getErrorMessage() + " */\n";
                    }
                    try (BufferedWriter writer = new BufferedWriter(new FileWriter(new File(outputDir, fileName)))) {
                        writer.write("/* entry: " + function.getEntryPoint() + " */\n");
                        writer.write(body);
                    }
                }
                index.write(node.root + "\t" + node.depth + "\t" + function.getEntryPoint() + "\t" +
                    function.getName() + "\t" + status + "\t" + fileName + "\n");
            }
        }
        decompiler.dispose();
        println("Exported " + nodes.size() + " call-tree rows to " + outputDir.getAbsolutePath());
    }
}

