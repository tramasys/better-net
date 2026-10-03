import json
from pathlib import Path
import subprocess
import sys


directory = Path(sys.argv[1])
commands = json.loads((directory / "compile_commands.json").read_text(encoding="utf-8"))
status = 0
for command in commands:
    if "src" not in Path(command["file"]).parts or Path(command["file"]).suffix != ".cpp":
        continue
    arguments = command.get("arguments")
    if arguments is None:
        import ctypes
        count = ctypes.c_int()
        function = ctypes.windll.shell32.CommandLineToArgvW
        function.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
        function.restype = ctypes.POINTER(ctypes.c_wchar_p)
        pointer = function(command["command"], ctypes.byref(count))
        arguments = list(pointer[:count.value])
        release = ctypes.windll.kernel32.LocalFree
        release.argtypes = [ctypes.c_void_p]
        release.restype = ctypes.c_void_p
        release(pointer)
    arguments = [value for value in arguments if value not in {"-c", "/c"} and not value.startswith(("/Fo", "/Fd"))]
    position = arguments.index("--") if "--" in arguments else len(arguments)
    arguments[position:position] = ["/clang:--analyze", "/clang:-Xanalyzer", "/clang:-analyzer-output=text"]
    status = max(status, subprocess.run(arguments, cwd=command["directory"]).returncode)
sys.exit(status)
