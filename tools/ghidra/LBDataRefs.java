// LBDataRefs.java - who mentions an ADDRESS, as data or as a literal.
//
// Part of legobattles-decomp's toolchain; the fourth of the set with LBDecomp,
// LBXrefs and LBForce.
//
// WHY THIS EXISTS. LBXrefs answers "who calls this function". The question that
// keeps coming up instead is "who touches this global", and on ARM that is a
// different search: a global is reached through a PC-relative literal, so the
// reference Ghidra records is often to the LITERAL POOL WORD rather than to the
// address itself. Grepping the binary for the four bytes finds those pools but
// not which function each belongs to, which is the part that matters.
//
// So this does both passes. First, direct references to the target address.
// Then, a scan for every defined word in the image whose VALUE is the target --
// the literal pools -- reporting the function containing each one. That second
// pass is what turns "0215802C is written by something" into a list of named
// callers to read.
//
// LB_TARGETS is the same file the others read: hex addresses, one per line,
// blank lines and # comments ignored. Output goes to stdout, because a
// reference list is a few lines and wants reading immediately.
//
// @category LEGOBattles

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.BufferedReader;
import java.io.FileReader;

public class LBDataRefs extends GhidraScript {

    private String where(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint();
    }

    @Override
    public void run() throws Exception {
        String targets = System.getenv("LB_TARGETS");
        if (targets == null) {
            println("LBDataRefs: set LB_TARGETS");
            return;
        }
        java.util.List<Long> wanted = new java.util.ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(targets))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                wanted.add(Long.parseLong(line.replace("0x", ""), 16));
            }
        }

        for (long w : wanted) {
            Address target = toAddr(w);
            println("LBDataRefs: " + target + " -- direct references");
            int n = 0;
            ReferenceIterator it = currentProgram.getReferenceManager()
                    .getReferencesTo(target);
            while (it.hasNext()) {
                Reference ref = it.next();
                println("LBDataRefs:    from " + ref.getFromAddress() + "  "
                        + ref.getReferenceType() + "  " + where(ref.getFromAddress()));
                ++n;
            }
            if (n == 0) println("LBDataRefs:    (none)");

            // THE LITERAL POOL PASS. On ARM a global is loaded through a
            // PC-relative word, so the instruction that uses it references the
            // POOL, not the global. Finding the pools means finding every word
            // in the image equal to the target -- and then, for each, asking
            // who references THAT, which is the instruction actually doing the
            // load.
            println("LBDataRefs: " + target + " -- literal pool words holding it");
            int pools = 0;
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                if (!b.isInitialized()) continue;
                Address a = b.getStart();
                Address end = b.getEnd();
                while (a.compareTo(end) < 0) {
                    try {
                        if ((getInt(a) & 0xFFFFFFFFL) == w) {
                            println("LBDataRefs:    word at " + a + "  " + where(a));
                            ReferenceIterator li = currentProgram.getReferenceManager()
                                    .getReferencesTo(a);
                            while (li.hasNext()) {
                                Reference lr = li.next();
                                println("LBDataRefs:       loaded by " + lr.getFromAddress()
                                        + "  " + lr.getReferenceType() + "  "
                                        + where(lr.getFromAddress()));
                            }
                            ++pools;
                        }
                    } catch (Exception ignored) {
                    }
                    a = a.add(4);
                    if (monitor.isCancelled()) return;
                }
            }
            if (pools == 0) println("LBDataRefs:    (none)");
        }
    }
}
