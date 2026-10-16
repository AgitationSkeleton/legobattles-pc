// LBDecomp.java - decompile the functions named in LB_TARGETS to C.
//
// Run as a POST-script. LB_TARGETS is a file of hex addresses, one per line
// (blank lines and # comments ignored); LB_OUTDIR is where the .c files go.
//
// This is the tool the project has been missing. Recovering the tilemap fill's
// loop bounds by hand-reading ARM took most of a session; the decompiler prints
// them. What it must NOT be used for is generating the port -- the sibling
// project's playbook/01 catalogues how Ghidra's C silently diverges from the
// binary (dropped call arguments at 169 of 170 call sites for one function).
// Read it to understand, then write the override against the DISASSEMBLY.
//
// @category LEGOBattles

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;

public class LBDecomp extends GhidraScript {

    @Override
    public void run() throws Exception {
        String targets = System.getenv("LB_TARGETS");
        String outdir = System.getenv("LB_OUTDIR");
        if (targets == null || outdir == null) {
            println("LBDecomp: set LB_TARGETS and LB_OUTDIR");
            return;
        }
        new File(outdir).mkdirs();

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);

        BufferedReader in = new BufferedReader(new FileReader(targets));
        String line;
        int ok = 0, miss = 0;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String hex = line.split("\\s+")[0].replace("0x", "");
            Address a;
            try {
                a = toAddr(Long.parseLong(hex, 16));
            } catch (NumberFormatException e) {
                continue;
            }
            Function fn = getFunctionAt(a);
            if (fn == null) fn = getFunctionContaining(a);
            if (fn == null) {
                println("LBDecomp: no function at " + hex);
                miss++;
                continue;
            }
            DecompileResults r = d.decompileFunction(fn, 120, monitor);
            String name = fn.getName() + "_" + hex;
            File out = new File(outdir, name + ".c");
            PrintWriter w = new PrintWriter(out, "UTF-8");
            w.println("// " + fn.getName() + " @ 0x" + hex
                      + "   entry " + fn.getEntryPoint()
                      + "   body " + fn.getBody().getNumAddresses() + " bytes");
            w.println("// Ghidra decompiler output. READ ONLY -- see"
                      + " playbook/01-what-ghidra-silently-drops.md before");
            w.println("// treating any of this as the function's real"
                      + " behaviour.");
            w.println();
            if (r != null && r.decompileCompleted()) {
                w.println(r.getDecompiledFunction().getC());
                ok++;
            } else {
                w.println("// DECOMPILATION FAILED: "
                          + (r == null ? "null" : r.getErrorMessage()));
                miss++;
            }
            w.close();
            println("LBDecomp: wrote " + out.getName());
        }
        in.close();
        d.dispose();
        println("LBDecomp: " + ok + " decompiled, " + miss + " failed");
    }
}
