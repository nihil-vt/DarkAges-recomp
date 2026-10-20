// Dump function list to JSON for N64Recomp symbol TOML generation.
// @category N64
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;

public class GhidraExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "symbols_ghidra.json";
        FunctionIterator funcs = currentProgram.getFunctionManager().getFunctions(true);

        int count = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            w.print("[\n");
            for (Function f : funcs) {
                w.format("  {\"name\": \"%s\", \"vram\": %d, \"size\": %d},\n",
                    f.getName(), f.getEntryPoint().getOffset(),
                    f.getBody().getNumAddresses());
                count++;
            }
            w.print("]\n");
        }
        println("Exported " + count + " functions to " + out);
    }
}
