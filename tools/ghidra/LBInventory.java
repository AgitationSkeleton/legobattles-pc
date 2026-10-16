// LBInventory.java - enumerate every function Ghidra knows about.
//
// Run as a POST-script. Writes a TSV to LB_INVENTORY (default
// lb_inventory.tsv in the home directory) with one row per function:
//
//     address  name  size  thunk  external  callers  called
//
// WHY THIS EXISTS. Every "percent decompiled" figure this project has quoted
// came from docs/data/decomp_status.tsv, which is a list of what HAS been
// done with no denominator attached. Counting its rows gives 791 functions and
// 3,404 bytes -- an average of 4.3 bytes each, which is a single-instruction
// accessor. Against the 1,378,432-byte ARM9 static section that is a quarter
// of one percent, and nobody had written that ratio down.
//
// A pivot to a native port is a decision about the denominator, so the
// denominator has to be measured rather than assumed: how many functions
// exist, how big they are, and how the size is distributed. A hundred
// four-byte getters and one 4 KB simulation step are not the same work.
//
// @category LEGOBattles
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;
import java.io.File;
import java.io.PrintWriter;

public class LBInventory extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = System.getenv("LB_INVENTORY");
        if (out == null || out.isEmpty()) {
            out = System.getProperty("user.home") + File.separator
                + "lb_inventory.tsv";
        }
        Listing listing = currentProgram.getListing();
        FunctionIterator it = listing.getFunctions(true);
        PrintWriter w = new PrintWriter(new File(out), "UTF-8");
        w.println("address\tname\tsize\tthunk\texternal\tcallers\tcalled");
        long n = 0, bytes = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            long size = f.getBody().getNumAddresses();
            // getNumAddresses is the count of addresses in the body, which for
            // a contiguous ARM function is its byte length. Thunks and
            // externals are flagged rather than dropped: they are real entries
            // in the call graph and excluding them silently would inflate any
            // coverage ratio computed from this file.
            w.print(f.getEntryPoint().toString());
            w.print('\t');
            w.print(f.getName());
            w.print('\t');
            w.print(size);
            w.print('\t');
            w.print(f.isThunk() ? 1 : 0);
            w.print('\t');
            w.print(f.isExternal() ? 1 : 0);
            w.print('\t');
            w.print(f.getCallingFunctions(monitor).size());
            w.print('\t');
            w.println(f.getCalledFunctions(monitor).size());
            ++n;
            bytes += size;
        }
        w.close();
        println("[LBInventory] " + n + " functions, " + bytes
                + " bytes of function body, written to " + out);
    }
}
