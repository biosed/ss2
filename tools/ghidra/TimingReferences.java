// Report static timing references in the primary game DLL. Output is local metadata.
//@category SS2
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import java.util.Set;
import java.util.TreeSet;

public class TimingReferences extends GhidraScript {
    private void collect(Address target, Set<Address> instructions, boolean followPointer) {
        for (Reference reference : getReferencesTo(target)) {
            Address source = reference.getFromAddress();
            if (getInstructionAt(source) != null) instructions.add(source);
            else if (followPointer && source.isMemoryAddress()) collect(source, instructions, false);
        }
    }

    @Override
    public void run() throws Exception {
        Address base = currentProgram.getImageBase();
        for (String name : new String[]{"GetTickCount", "timeGetTime", "SetTimer", "Sleep"}) {
            Set<Address> sites = new TreeSet<>();
            for (Symbol symbol : currentProgram.getSymbolTable().getSymbols(name)) {
                collect(symbol.getAddress(), sites, true);
            }
            println("SS2 " + name + " instruction references=" + sites.size());
            for (Address site : sites) {
                Function owner = getFunctionContaining(site);
                println(String.format("SS2 %s siteRVA=0x%x function=%s", name, site.subtract(base),
                    owner == null ? "unknown" : owner.getEntryPoint().toString()));
            }
        }
        // Check that the native decompiler can analyze a known entry point.
        for (Symbol symbol : currentProgram.getSymbolTable().getSymbols("VModule_Play")) {
            Function play = getFunctionAt(symbol.getAddress());
            if (play == null) continue;
            DecompInterface decompiler = new DecompInterface();
            try {
                if (!decompiler.openProgram(currentProgram) ||
                    !decompiler.decompileFunction(play, 60, monitor).decompileCompleted()) {
                    throw new IllegalStateException("VModule_Play decompilation failed");
                }
                println(String.format("SS2 decompiled VModule_Play RVA=0x%x", symbol.getAddress().subtract(base)));
                return;
            } finally {
                decompiler.dispose();
            }
        }
        throw new IllegalStateException("VModule_Play export missing");
    }
}
