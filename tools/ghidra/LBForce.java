// LBForce.java - create a function where Ghidra found none, then decompile it.
//
// Part of legobattles-decomp's toolchain; the third of the trio with LBDecomp
// and LBXrefs.
//
// WHY. The analysis pass leaves stretches of this binary unclaimed, and the
// investigation keeps landing in them: the caller of the entity gather at
// 020e3400, and the routine at 020D48CC that the selection trace pointed at,
// are both "no function" as far as Ghidra is concerned even though the code is
// resident and executing. LBDecomp declines those, correctly, because it will
// not guess at a function boundary.
//
// This one is told the boundary. LB_TARGETS holds the ENTRY addresses -- found
// by scanning back to a Thumb push, which is how 020D48CC was identified -- and
// this disassembles from there, creates the function, and decompiles it.
//
// THUMB MATTERS AND IS NOT GUESSED. ARM's instruction set is per-address state:
// disassembling Thumb code as ARM produces confident, plausible, wrong output,
// and half the functions this port cares about are Thumb sitting next to ARM
// ones. An address in LB_TARGETS may be followed by the word "arm" or "thumb";
// thumb is the default here because every address this has been needed for so
// far has been Thumb, and the TMode context register is set explicitly either
// way rather than left to inference.
//
// @category LEGOBattles

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.util.CodeUnitInsertionException;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.math.BigInteger;

public class LBForce extends GhidraScript {

    @Override
    public void run() throws Exception {
        String targets = System.getenv("LB_TARGETS");
        String outdir = System.getenv("LB_OUTDIR");
        if (targets == null || outdir == null) {
            println("LBForce: set LB_TARGETS and LB_OUTDIR");
            return;
        }
        new File(outdir).mkdirs();

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Register tmode = currentProgram.getRegister("TMode");

        BufferedReader in = new BufferedReader(new FileReader(targets));
        String line;
        int ok = 0, miss = 0;
        while ((line = in.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] tok = line.split("\\s+");
            String hex = tok[0].replace("0x", "");
            boolean arm = line.toLowerCase().contains(" arm");
            Address a;
            try {
                a = toAddr(Long.parseLong(hex, 16));
            } catch (NumberFormatException e) {
                continue;
            }
            Function fn = getFunctionAt(a);
            if (fn == null) {
                if (tmode != null) {
                    try {
                        currentProgram.getProgramContext().setValue(
                            tmode, a, a,
                            BigInteger.valueOf(arm ? 0 : 1));
                    } catch (Exception e) {
                        println("LBForce: TMode set failed at " + hex
                                + ": " + e);
                    }
                }
                disassemble(a);
                try {
                    fn = createFunction(a, null);
                } catch (Exception e) {
                    println("LBForce: createFunction failed at " + hex
                            + ": " + e);
                }
            }
            if (fn == null) {
                println("LBForce: still no function at " + hex);
                miss++;
                continue;
            }
            DecompileResults r = d.decompileFunction(fn, 120, monitor);
            String name = fn.getName() + "_" + hex;
            PrintWriter w = new PrintWriter(new File(outdir, name + ".c"),
                                            "UTF-8");
            w.println("// " + fn.getName() + " @ 0x" + hex
                      + "   FORCED (Ghidra had no function here)"
                      + "   " + (arm ? "arm" : "thumb")
                      + "   body " + fn.getBody().getNumAddresses() + " bytes");
            w.println("// Ghidra decompiler output. READ ONLY -- and doubly so"
                      + " here: the function boundary was");
            w.println("// asserted by hand, so a wrong entry gives confident"
                      + " nonsense. Check it against the");
            w.println("// disassembly before believing any of it.");
            w.println();
            w.println(r.getDecompiledFunction() == null
                      ? "// decompilation failed"
                      : r.getDecompiledFunction().getC());
            w.close();
            println("LBForce: wrote " + name + ".c");
            ok++;
        }
        in.close();
        println("LBForce: " + ok + " decompiled, " + miss + " failed");
    }
}
