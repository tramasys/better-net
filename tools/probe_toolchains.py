from pathlib import Path
import shutil
import subprocess


root = Path(__file__).resolve().parents[1]
output = root / "build" / "probes"
output.mkdir(parents=True, exist_ok=True)
for compiler, mode, name in (("clang-cl", "/clang:-std=c++26", "clang-cxx26"), ("cl", "/std:c++latest", "msvc-latest")):
    executable = output / f"{name}.exe"
    command = [
        shutil.which(compiler) or compiler, "/nologo", mode, "/EHsc", "/MT", "/utf-8", "/Zc:__cplusplus",
        str(root / "tests" / "feature_probe.cpp"), f"/Fe:{executable}", f"/Fo:{output / (name + '.obj')}",
        "/link", "d2d1.lib", "dwrite.lib", "windowscodecs.lib", "ole32.lib",
    ]
    subprocess.run(command, check=True, cwd=output)
    subprocess.run([str(executable)], check=True, cwd=output)
    print(f"{compiler} {mode}: compiled, linked, and ran successfully")
