// LBDecompV2.java - decompile EVERY function in the program to C (no target list needed).
//
// Output dir: first script argument, else env LB_OUTDIR, else a built-in default.
// Files: <name>_<entry>.c  (e.g. NitroMain_020f6730.c, FUN_02001234_02001234.c)
// Also writes index.tsv: entry  name  body_bytes  status(ok|FAILED)  file
//
// Same caveat as LBDecomp.java: Ghidra's C is for reading, not for generating the port.
//
// @category LEGOBattles

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class LBDecompV2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outdir = (args != null && args.length > 0) ? args[0] : System.getenv("LB_OUTDIR");
        if (outdir == null) outdir = "D:\\RETools\\decomp_all_v2";
        new File(outdir).mkdirs();

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        int ok = 0, fail = 0;
        try (PrintWriter idx = new PrintWriter(new File(outdir, "index.tsv"), "UTF-8")) {
            idx.println("entry\tname\tbody_bytes\tstatus\tfile");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function fn = it.next();
                String hex = fn.getEntryPoint().toString();
                String safe = fn.getName().replaceAll("[^A-Za-z0-9_]", "_");
                String fname = safe + "_" + hex + ".c";
                DecompileResults r = d.decompileFunction(fn, 120, monitor);
                boolean good = r != null && r.decompileCompleted();
                try (PrintWriter w = new PrintWriter(new File(outdir, fname), "UTF-8")) {
                    w.println("// " + fn.getName() + " @ 0x" + hex
                              + "   body " + fn.getBody().getNumAddresses() + " bytes");
                    String c = fn.getComment();
                    if (c != null && !c.isEmpty()) {
                        for (String l : c.split("\n")) w.println("// " + l);
                    }
                    w.println("// Ghidra decompiler output. READ ONLY -- see"
                              + " playbook/01-what-ghidra-silently-drops.md before");
                    w.println("// treating any of this as the function's real behaviour.");
                    w.println();
                    if (good) {
                        w.println(r.getDecompiledFunction().getC());
                    } else {
                        w.println("// DECOMPILATION FAILED: " + (r == null ? "null" : r.getErrorMessage()));
                    }
                }
                if (good) ok++; else { fail++; println("LBDecompV2: FAILED " + hex + " " + fn.getName()); }
                idx.println(hex + "\t" + fn.getName() + "\t" + fn.getBody().getNumAddresses() + "\t"
                            + (good ? "ok" : "FAILED") + "\t" + fname);
                if ((ok + fail) % 500 == 0) println("LBDecompV2: progress " + (ok + fail));
            }
        }
        d.dispose();
        println("LBDecompV2: " + ok + " decompiled, " + fail + " failed, total " + (ok + fail));
    }
}
