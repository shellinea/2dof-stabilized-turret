// Ghidra headless post-script: decompile every function to $GHIDRA_OUT.
// Optional $GHIDRA_FILTER: only emit functions whose name contains the substring.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = System.getenv("GHIDRA_OUT");
        if (outPath == null || outPath.isEmpty()) outPath = "decompiled_all.c";
        String filter = System.getenv("GHIDRA_FILTER");
        if (filter == null) filter = "";

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);

        PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(new File(outPath))));
        FunctionManager fm = currentProgram.getFunctionManager();
        int count = 0, total = 0;
        for (Function f : fm.getFunctions(true)) {
            total++;
            String name = f.getName();
            if (!filter.isEmpty() && !name.contains(filter)) continue;
            try {
                DecompileResults res = ifc.decompileFunction(f, 30, monitor);
                if (res != null && res.decompileCompleted()) {
                    w.println("// ===== " + name + " @ " + f.getEntryPoint() + " =====");
                    w.println(res.getDecompiledFunction().getC());
                    w.println();
                    count++;
                }
            } catch (Exception e) {
                // skip unliftable functions (Enigma VM stubs, bad data)
            }
        }
        w.close();
        println("GHIDRA: dumped " + count + " / " + total + " functions to " + outPath);
    }
}
