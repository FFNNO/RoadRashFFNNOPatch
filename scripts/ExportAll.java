// Export functions, imports, strings and decompiled C for offline analysis.
// @category RoadRash
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.util.*;

public class ExportAll extends GhidraScript {

    private String fnAt(Address a) {
        Function f = getFunctionContaining(a);
        return f == null ? "?" : f.getEntryPoint() + ":" + f.getName();
    }

    @Override
    protected void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        new File(outDir).mkdirs();
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();

        // Imports with calling functions
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(outDir, "imports.tsv")))) {
            for (Symbol s : currentProgram.getSymbolTable().getExternalSymbols()) {
                Set<String> callers = new TreeSet<>();
                for (Reference r : s.getReferences()) {
                    Address from = r.getFromAddress();
                    for (Reference r2 : refs.getReferencesTo(from)) callers.add(fnAt(r2.getFromAddress()));
                    callers.add(fnAt(from));
                }
                callers.remove("?");
                w.println(s.getParentNamespace().getName() + "\t" + s.getName() + "\t" + String.join(",", callers));
            }
        }

        // Defined strings with referencing functions
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(outDir, "strings.tsv")))) {
            DataIterator it = listing.getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                String v = StringDataInstance.getStringDataInstance(d).getStringValue();
                if (v == null) continue;
                Set<String> users = new TreeSet<>();
                for (Reference r : refs.getReferencesTo(d.getAddress())) users.add(fnAt(r.getFromAddress()));
                w.println(d.getAddress() + "\t" + v.replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r") + "\t" + String.join(",", users));
            }
        }

        // Functions + decompilation
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter fw = new PrintWriter(new FileWriter(new File(outDir, "functions.tsv")));
             PrintWriter cw = new PrintWriter(new FileWriter(new File(outDir, "decomp.c")))) {
            FunctionIterator fi = listing.getFunctions(true);
            while (fi.hasNext() && !monitor.isCancelled()) {
                Function f = fi.next();
                int callers = f.getCallingFunctions(monitor).size();
                int callees = f.getCalledFunctions(monitor).size();
                fw.println(f.getEntryPoint() + "\t" + f.getName() + "\t" + f.getBody().getNumAddresses() + "\t" + callers + "\t" + callees);
                if (f.isThunk() || f.isExternal()) continue;
                DecompileResults res = ifc.decompileFunction(f, 60, monitor);
                cw.println("// ==== FUNC " + f.getEntryPoint() + " " + f.getName() + " size=" + f.getBody().getNumAddresses());
                if (res != null && res.decompileCompleted()) cw.println(res.getDecompiledFunction().getC());
                else cw.println("// decompile failed");
            }
        }
        ifc.dispose();
        println("Export done: " + outDir);
    }
}
