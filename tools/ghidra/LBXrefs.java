// LBXrefs.java - who CALLS the functions named in LB_TARGETS.
//
// Part of legobattles-decomp's toolchain; a sibling of LBDecomp.java.
//
// WHY THIS EXISTS. The open question in adaptive widescreen is why a click in the
// widened band does not select a unit. Five causes have been excluded by
// measurement (docs/226), so the decision is downstream of all of them, and the
// way to find it is to identify the routine that marks a unit selected and then
// walk BACKWARDS through its callers looking for the test that refuses.
//
// Walking backwards by hand means grepping a 1.4 MB binary for BL encodings and
// decoding each one's target, which is exactly the sort of thing that produces a
// confident wrong answer at 3 a.m. Ghidra already has the call graph from the
// analysis pass; this just prints it.
//
// Reads the same LB_TARGETS file LBDecomp does -- hex addresses, one per line,
// blank lines and # comments ignored -- and writes to stdout rather than to
// files, because a caller list is a few lines and wants reading immediately.
//
// @category LEGOBattles

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.BufferedReader;
import java.io.FileReader;

public class LBXrefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String targets = System.getenv("LB_TARGETS");
        if (targets == null) {
            println("LBXrefs: set LB_TARGETS");
            return;
        }
        BufferedReader in = new BufferedReader(new FileReader(targets));
        String line;
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
                println("LBXrefs: no function at " + hex);
                continue;
            }
            println("LBXrefs: " + fn.getName() + " @ " + fn.getEntryPoint()
                    + "  (asked for " + hex + ")");
            ReferenceIterator it =
                currentProgram.getReferenceManager()
                    .getReferencesTo(fn.getEntryPoint());
            int n = 0;
            while (it.hasNext()) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function caller = getFunctionContaining(from);
                println("LBXrefs:    from " + from
                        + "  " + r.getReferenceType()
                        + (caller == null ? "  (no function)"
                                          : "  in " + caller.getName()
                                            + " @ " + caller.getEntryPoint()));
                n++;
            }
            if (n == 0) println("LBXrefs:    (no references)");
            println("LBXrefs:    " + n + " reference(s)");
        }
        in.close();
    }
}
