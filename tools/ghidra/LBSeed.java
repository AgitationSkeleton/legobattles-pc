// LBSeed.java - seed Ghidra with the entry points this project already knows.
//
// Run as a PRE-script, before auto-analysis.
//
// The corpus in out/eur/corpus.tsv carries 9,315 function starts WITH their
// ARM/Thumb mode, recovered by this project's own scanner. Handing them to
// Ghidra before analysis matters more on ARM than on most targets: the
// instruction set is per-address state, and a decompiler that guesses Thumb
// where the code is ARM produces confident, plausible, wrong C. Half the
// functions this port cares about are Thumb (func_020A3234, func_020A3478),
// sitting immediately next to ARM ones.
//
// @category LEGOBattles

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.FileReader;
import java.math.BigInteger;

public class LBSeed extends GhidraScript {

    @Override
    public void run() throws Exception {
        String path = System.getenv("LB_CORPUS");
        if (path == null) {
            println("LBSeed: set LB_CORPUS to corpus.tsv");
            return;
        }

        Register tmode = currentProgram.getRegister("TMode");
        long base = currentProgram.getImageBase().getOffset();
        long end = base + currentProgram.getMemory().getSize();

        int seeded = 0, thumb = 0, skipped = 0;
        BufferedReader in = new BufferedReader(new FileReader(path));
        String line = in.readLine();                 // header
        while ((line = in.readLine()) != null) {
            String[] f = line.split("\t");
            if (f.length < 5) continue;
            long ea;
            try {
                ea = Long.parseLong(f[0].replace("0x", ""), 16);
            } catch (NumberFormatException e) {
                continue;
            }
            // Only this image. The corpus spans ITCM, DTCM and 91 overlays that
            // all load at the same address; importing them into one space would
            // be worse than useless.
            if (ea < base || ea >= end) { skipped++; continue; }

            Address a = toAddr(ea);
            boolean isThumb = "thumb".equalsIgnoreCase(f[1]);
            if (tmode != null) {
                currentProgram.getProgramContext().setRegisterValue(
                    a, a, new RegisterValue(tmode,
                        BigInteger.valueOf(isThumb ? 1 : 0)));
                if (isThumb) thumb++;
            }
            disassemble(a);
            Function fn = getFunctionAt(a);
            if (fn == null) {
                fn = createFunction(a, f[4]);
            }
            if (fn != null && f[4] != null && !f[4].isEmpty()) {
                fn.setName(f[4], SourceType.IMPORTED);
            }
            seeded++;
            if ((seeded % 1000) == 0) println("LBSeed: " + seeded + " seeded");
        }
        in.close();
        println("LBSeed: seeded " + seeded + " functions (" + thumb
                + " thumb), skipped " + skipped + " outside this image");
    }
}
