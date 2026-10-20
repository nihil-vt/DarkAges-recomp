// Gap-fill: find missed functions via pointer tables + IDO prologue patterns.
// Run AFTER import analysis, BEFORE GhidraExportFunctions.
// @category N64
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.SourceType;

public class GhidraGapFill extends GhidraScript {
    static final long T0 = 0x80001000L;
    static final long T1 = 0x80001000L + 0x7D200;

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();

        int created;
        int total = 0;
        do {
            created = 0;
            // Build gap list from current functions.
            AddressSet covered = new AddressSet();
            for (Function f : listing.getFunctions(true)) {
                covered.add(f.getBody());
            }
            AddressSet gaps = new AddressSet();
            gaps.add(currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(T0),
                     currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(T1 - 1));
            gaps.delete(covered);

            // Collect candidate starts: pointer values into gaps + IDO prologues in gaps.
            java.util.TreeSet<Long> cands = new java.util.TreeSet<>();

            // Pass A: any word in memory whose value points into a gap.
            for (MemoryBlock b : mem.getBlocks()) {
                if (!b.isInitialized()) continue;
                long start = Math.max(b.getStart().getOffset(), T0 - 0x200000);
                long end = Math.min(b.getEnd().getOffset() + 1, T1);
                byte[] buf = new byte[4];
                for (long a = start & ~3; a < end; a += 4) {
                    try {
                        mem.getBytes(toAddr(a), buf);
                    } catch (Exception e) { continue; }
                    long v = ((long)(buf[0] & 0xFF) << 24) | ((buf[1] & 0xFF) << 16)
                            | ((buf[2] & 0xFF) << 8) | (buf[3] & 0xFF);
                    if (v >= T0 && v < T1 && (v & 3) == 0 && gaps.contains(toAddr(v))) {
                        cands.add(v);
                    }
                }
            }

            // Pass B: IDO prologues inside gaps: addiu sp,sp,imm (0x27BDxxxx)
            // preceded by typical stack-size setup, with sw ra within first 8 words.
            for (AddressRange r : gaps) {
                byte[] buf = new byte[(int)(r.getLength() & ~3)];
                if (buf.length < 8) continue;
                mem.getBytes(r.getMinAddress(), buf);
                for (int i = 0; i + 8 < buf.length; i += 4) {
                    long w = ((long)(buf[i] & 0xFF) << 24) | ((buf[i+1] & 0xFF) << 16)
                            | ((buf[i+2] & 0xFF) << 8) | (buf[i+3] & 0xFF);
                    if ((w >>> 16) != 0x27BD) continue;  // addiu sp,sp
                    // look for sw ra, N(sp) = AFBxxxxx within next 4 instrs
                    boolean ok = false;
                    for (int j = 4; j <= 16 && i + j + 4 <= buf.length; j += 4) {
                        long w2 = ((long)(buf[i+j] & 0xFF) << 24) | ((buf[i+j+1] & 0xFF) << 16)
                                | ((buf[i+j+2] & 0xFF) << 8) | (buf[i+j+3] & 0xFF);
                        if ((w2 >>> 16) == 0xAFBF) { ok = true; break; }
                    }
                    if (ok) {
                        long addr = r.getMinAddress().getOffset() + i;
                        if (gaps.contains(toAddr(addr))) cands.add(addr);
                    }
                }
            }

            // Create functions at candidates not inside existing functions.
            for (long c : cands) {
                Address addr = toAddr(c);
                if (listing.getFunctionAt(addr) != null) continue;
                if (listing.getInstructionAt(addr) == null) {
                    if (!disassemble(addr)) continue;
                }
                createFunction(addr, null);
                created++;
            }
            total += created;
            println("pass created " + created + " (" + cands.size() + " cands)");
        } while (created > 0);
        println("Gap-fill total created: " + total);
    }
}
