// LBApplyRtti.java - name every virtual function the cartridge's own RTTI
// identifies, from docs/data/vtable_methods.tsv (written by tools/lb_rtti.py).
//
// Input: first script argument, else env LB_RTTI_TSV. Tab-separated with a
// header row; the columns used are class_name, slot_index, function_address
// (0x-prefixed hex) and mode (arm|thumb).
//
// A function in several vtables (an inherited or shared implementation) gets
// ONE name, from the first row that lists it, and a comment naming every
// class and slot that holds it. The name drops a leading "Battles::" and
// joins the rest with "__": Battles::User::PauseMenu slot 8 becomes
// User__PauseMenu__v08. Names a person gave (SourceType.USER_DEFINED) are
// kept, and still get the comment. An address with no function is
// disassembled in the mode the TSV records and a function created there.
//
// Output lines "LBApplyRtti: <STATUS> ..." go to the script log.
//
// @category LEGOBattles

import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class LBApplyRtti extends GhidraScript {
    private static String shortName(String cls, int slot) {
        String c = cls.startsWith("Battles::") ? cls.substring(9) : cls;
        c = c.replace("::", "__").replaceAll("[^A-Za-z0-9_]", "_");
        return String.format("%s__v%02d", c, slot);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String path = (args != null && args.length > 0) ? args[0] : System.getenv("LB_RTTI_TSV");
        if (path == null) {
            println("LBApplyRtti: no TSV given (script argument or LB_RTTI_TSV)");
            return;
        }
        // address -> every (class, slot, mode) that holds it, in file order
        Map<Long, List<String[]>> slots = new LinkedHashMap<>();
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String header = in.readLine();
            if (header == null) return;
            String[] h = header.split("\t");
            int iCls = -1, iSlot = -1, iAddr = -1, iMode = -1;
            for (int i = 0; i < h.length; ++i) {
                if (h[i].equals("class_name")) iCls = i;
                else if (h[i].equals("slot_index")) iSlot = i;
                else if (h[i].equals("function_address")) iAddr = i;
                else if (h[i].equals("mode")) iMode = i;
            }
            if (iCls < 0 || iSlot < 0 || iAddr < 0 || iMode < 0) {
                println("LBApplyRtti: header lacks class_name/slot_index/function_address/mode");
                return;
            }
            String line;
            while ((line = in.readLine()) != null) {
                String[] p = line.split("\t", -1);
                if (p.length <= Math.max(Math.max(iCls, iSlot), Math.max(iAddr, iMode))) continue;
                String a = p[iAddr].trim();
                if (a.isEmpty()) continue;
                long addr = Long.parseLong(a.startsWith("0x") ? a.substring(2) : a, 16) & ~1L;
                if (addr == 0) continue;
                slots.computeIfAbsent(addr, k -> new ArrayList<>())
                     .add(new String[] {p[iCls].trim(), p[iSlot].trim(), p[iMode].trim()});
            }
        }
        int named = 0, kept = 0, created = 0, skipped = 0;
        for (Map.Entry<Long, List<String[]>> e : slots.entrySet()) {
            if (monitor.isCancelled()) break;
            Address a = toAddr(e.getKey());
            List<String[]> held = e.getValue();
            String[] first = held.get(0);
            String name = shortName(first[0], Integer.parseInt(first[1]));
            StringBuilder plate = new StringBuilder("RTTI vtable slot:");
            for (String[] s : held)
                plate.append(String.format(" %s::virtual_%02d", s[0], Integer.parseInt(s[1])));
            plate.append("\n[evidence: rtti; source: docs/data/vtable_methods.tsv]");

            Function f = getFunctionAt(a);
            if (f == null) {
                Function c = getFunctionContaining(a);
                if (c != null) {
                    println("LBApplyRtti: SKIP-INSIDE " + a + " " + name + " (inside "
                            + c.getName() + " @ " + c.getEntryPoint() + ")");
                    skipped++;
                    continue;
                }
                if (currentProgram.getListing().getInstructionAt(a) == null) {
                    boolean thumb = first[2].equalsIgnoreCase("thumb");
                    new ArmDisassembleCommand(a, null, thumb).applyTo(currentProgram, monitor);
                }
                if (currentProgram.getListing().getInstructionAt(a) == null) {
                    println("LBApplyRtti: SKIP-NODISASM " + a + " " + name);
                    skipped++;
                    continue;
                }
                f = createFunction(a, null);
                if (f == null) {
                    println("LBApplyRtti: SKIP-NOCREATE " + a + " " + name);
                    skipped++;
                    continue;
                }
                created++;
                println("LBApplyRtti: CREATED " + a + " body " + f.getBody().getNumAddresses());
            }
            String prev = f.getComment();
            if (prev == null || !prev.contains("[evidence: rtti;")) {
                f.setComment(prev == null || prev.isEmpty() ? plate.toString()
                                                             : prev + "\n" + plate);
            }
            if (f.getSymbol().getSource() == SourceType.USER_DEFINED
                    && !f.getName().equals(name)) {
                kept++;
                println("LBApplyRtti: KEEP " + a + " " + f.getName() + " (rtti " + name + ")");
                continue;
            }
            try {
                f.setName(name, SourceType.USER_DEFINED);
            } catch (ghidra.util.exception.DuplicateNameException dup) {
                // A class with more than one vtable (multiple inheritance)
                // can hold the same slot number at two addresses.
                f.setName(name + "_" + a.toString(), SourceType.USER_DEFINED);
            }
            named++;
        }
        println("LBApplyRtti: " + slots.size() + " addresses; named " + named + " (created "
                + created + "), kept " + kept + " existing names, skipped " + skipped);
    }
}
