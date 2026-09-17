# Ghidra headless post-script (Jython 2.7 / Ghidra 11-12)
# Decompile every function and dump readable C to $GHIDRA_OUT.
# Optional $GHIDRA_FILTER: only emit functions whose name contains this substring.

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
import os

out_path = os.environ.get("GHIDRA_OUT", "decompiled.c")
filt = os.environ.get("GHIDRA_FILTER", "")

prog = currentProgram
ifc = DecompInterface()
ifc.openProgram(prog)
mon = ConsoleTaskMonitor()

fm = prog.getFunctionManager()
funcs = fm.getFunctions(True)

out = open(out_path, "w")
count = 0
total = 0
for f in funcs:
    total += 1
    name = f.getName()
    if filt and filt not in name:
        continue
    try:
        res = ifc.decompileFunction(f, 30, mon)
    except Exception as e:
        continue
    if res is None or not res.decompileCompleted():
        continue
    code = res.getDecompiledFunction().getC()
    out.write("// ===== %s @ %s =====\n" % (name, f.getEntryPoint()))
    out.write(code)
    out.write("\n\n")
    count += 1

out.close()
printf = None
print("GHIDRA: dumped %d / %d functions to %s" % (count, total, out_path))
