import argparse
import bisect
import ctypes as c
import difflib
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
VERSION = "23.1.2"
SCOPES = {2, 3, 4, 5, 22, 23, 31, 32, 202, 350}
SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".hxx"}


class String(c.Structure):
    _fields_ = [("data", c.c_void_p), ("flags", c.c_uint)]


class Cursor(c.Structure):
    _fields_ = [("kind", c.c_uint), ("xdata", c.c_int), ("data", c.c_void_p * 3)]


class Location(c.Structure):
    _fields_ = [("data", c.c_void_p * 2), ("value", c.c_uint)]


class Range(c.Structure):
    _fields_ = [("data", c.c_void_p * 2), ("begin", c.c_uint), ("end", c.c_uint)]


class Token(c.Structure):
    _fields_ = [("data", c.c_uint * 4), ("pointer", c.c_void_p)]


class Unsaved(c.Structure):
    _fields_ = [("name", c.c_char_p), ("contents", c.c_char_p), ("length", c.c_ulong)]


VISITOR = c.CFUNCTYPE(c.c_uint, Cursor, Cursor, c.c_void_p)


class Parser:
    def __init__(self):
        self.formatter = shutil.which("clang-format")
        compiler = shutil.which("clang-cl")
        if not self.formatter or not compiler:
            raise RuntimeError("clang-format and clang-cl 23.1.2 must be on PATH")
        version = subprocess.check_output([self.formatter, "--version"], text=True)
        if version.split()[2] != VERSION:
            raise RuntimeError(f"Expected clang-format {VERSION}; received {version.strip()}")
        library = Path(os.environ.get("BNET_LIBCLANG", str(Path(compiler).parent / "libclang.dll")))
        self.dll = c.CDLL(str(library))
        declarations = {
            "clang_getClangVersion": (String, []),
            "clang_getCString": (c.c_char_p, [String]),
            "clang_disposeString": (None, [String]),
            "clang_createIndex": (c.c_void_p, [c.c_int, c.c_int]),
            "clang_disposeIndex": (None, [c.c_void_p]),
            "clang_parseTranslationUnit": (
                c.c_void_p,
                [c.c_void_p, c.c_char_p, c.POINTER(c.c_char_p), c.c_int, c.POINTER(Unsaved), c.c_uint, c.c_uint],
            ),
            "clang_disposeTranslationUnit": (None, [c.c_void_p]),
            "clang_getTranslationUnitCursor": (Cursor, [c.c_void_p]),
            "clang_visitChildren": (c.c_uint, [Cursor, VISITOR, c.c_void_p]),
            "clang_getCursorExtent": (Range, [Cursor]),
            "clang_getRangeStart": (Location, [Range]),
            "clang_getRangeEnd": (Location, [Range]),
            "clang_getSpellingLocation": (
                None,
                [Location, c.POINTER(c.c_void_p), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint)],
            ),
            "clang_getFile": (c.c_void_p, [c.c_void_p, c.c_char_p]),
            "clang_getLocationForOffset": (Location, [c.c_void_p, c.c_void_p, c.c_uint]),
            "clang_getRange": (Range, [Location, Location]),
            "clang_tokenize": (None, [c.c_void_p, Range, c.POINTER(c.POINTER(Token)), c.POINTER(c.c_uint)]),
            "clang_disposeTokens": (None, [c.c_void_p, c.POINTER(Token), c.c_uint]),
            "clang_getTokenSpelling": (String, [c.c_void_p, Token]),
            "clang_getTokenExtent": (Range, [c.c_void_p, Token]),
            "clang_getNumDiagnostics": (c.c_uint, [c.c_void_p]),
            "clang_getDiagnostic": (c.c_void_p, [c.c_void_p, c.c_uint]),
            "clang_getDiagnosticSeverity": (c.c_uint, [c.c_void_p]),
            "clang_formatDiagnostic": (String, [c.c_void_p, c.c_uint]),
            "clang_disposeDiagnostic": (None, [c.c_void_p]),
        }
        for name, (result, arguments) in declarations.items():
            function = getattr(self.dll, name)
            function.restype = result
            function.argtypes = arguments
        version = self.string(self.dll.clang_getClangVersion())
        if f"clang version {VERSION} " not in version + " ":
            raise RuntimeError(f"Expected libclang {VERSION}; received {version}")
        resource = subprocess.check_output([compiler, "/clang:-print-resource-dir"], text=True).strip()
        self.arguments = [
            "-x", "c++", "-std=c++26", "-fms-extensions", "-fms-compatibility", "-fms-compatibility-version=20",
            "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-Wno-pragma-once-outside-header",
            "-resource-dir", resource, "-I", str(ROOT / "src"),
        ]
        for directory in os.environ.get("INCLUDE", "").split(";"):
            if directory:
                self.arguments += ["-isystem", directory]
        self.index = self.dll.clang_createIndex(0, 0)

    def string(self, value):
        result = self.dll.clang_getCString(value)
        text = result.decode("utf-8", errors="replace") if result else ""
        self.dll.clang_disposeString(value)
        return text

    def parse(self, path, contents):
        name = str(path.resolve()).encode("utf-8")
        arguments = (c.c_char_p * len(self.arguments))(*(value.encode("utf-8") for value in self.arguments))
        unsaved = Unsaved(name, contents, len(contents))
        unit = self.dll.clang_parseTranslationUnit(self.index, name, arguments, len(arguments), c.byref(unsaved), 1, 1)
        if not unit:
            raise RuntimeError(f"libclang could not parse {path}")
        errors = []
        for number in range(self.dll.clang_getNumDiagnostics(unit)):
            diagnostic = self.dll.clang_getDiagnostic(unit, number)
            if self.dll.clang_getDiagnosticSeverity(diagnostic) >= 3:
                errors.append(self.string(self.dll.clang_formatDiagnostic(diagnostic, 3)))
            self.dll.clang_disposeDiagnostic(diagnostic)
        if errors:
            self.dll.clang_disposeTranslationUnit(unit)
            raise RuntimeError("Spacing pass requires valid C++:\n" + "\n".join(errors[:8]))
        return unit, self.dll.clang_getFile(unit, name)

    def offset(self, location):
        file = c.c_void_p()
        offset = c.c_uint()
        self.dll.clang_getSpellingLocation(location, c.byref(file), None, None, c.byref(offset))
        return file.value, offset.value

    def extent(self, extent):
        first_file, first = self.offset(self.dll.clang_getRangeStart(extent))
        last_file, last = self.offset(self.dll.clang_getRangeEnd(extent))
        return first_file, last_file, first, last

    def tokens(self, unit, file, size):
        extent = self.dll.clang_getRange(
            self.dll.clang_getLocationForOffset(unit, file, 0),
            self.dll.clang_getLocationForOffset(unit, file, size),
        )
        tokens = c.POINTER(Token)()
        count = c.c_uint()
        self.dll.clang_tokenize(unit, extent, c.byref(tokens), c.byref(count))
        result = []
        for number in range(count.value):
            token = tokens[number]
            _, _, first, last = self.extent(self.dll.clang_getTokenExtent(unit, token))
            result.append((self.string(self.dll.clang_getTokenSpelling(unit, token)), first, last))
        self.dll.clang_disposeTokens(unit, tokens, count)
        return result

    def children(self, cursor):
        result = []

        @VISITOR
        def visitor(child, parent, context):
            result.append(child)
            return 1

        self.dll.clang_visitChildren(cursor, visitor, None)
        return result

    def scopes(self, unit, file):
        result = []

        def visit(cursor):
            children = []
            for child in self.children(cursor):
                first_file, last_file, first, last = self.extent(self.dll.clang_getCursorExtent(child))
                if first_file == file and last_file == file and first < last and child.kind < 500:
                    children.append((child, first, last))
            if cursor.kind in SCOPES:
                items = [
                    (first, last) for child, first, last in children
                    if cursor.kind == 202 or 1 <= child.kind < 40 and child.kind not in {27, 28, 29}
                ]
                result.append(items)
            for child, first, last in children:
                visit(child)

        visit(self.dll.clang_getTranslationUnitCursor(unit))
        return result


def directives(contents, tokens):
    result = []
    for number, (spelling, first, last) in enumerate(tokens):
        line_start = contents.rfind(b"\n", 0, first) + 1
        if spelling != "#" or contents[line_start:first].strip():
            continue
        end = contents.find(b"\n", last)
        if end == -1:
            end = len(contents)
        while contents[line_start:end].rstrip().endswith(b"\\"):
            end = contents.find(b"\n", end + 1)
            if end == -1:
                end = len(contents)
                break
        keyword = tokens[number + 1][0] if number + 1 < len(tokens) else ""
        result.append((keyword, first, end))
    return result


def spacing(parser, path, contents):
    unit, file = parser.parse(path, contents)
    try:
        tokens = parser.tokens(unit, file, len(contents))
        scopes = parser.scopes(unit, file)
    finally:
        parser.dll.clang_disposeTranslationUnit(unit)
    commands = directives(contents, tokens)
    conditions = [
        (keyword, first, last) for keyword, first, last in commands
        if keyword in {"if", "ifdef", "ifndef", "elif"}
    ]
    if len(conditions) > 5:
        raise RuntimeError(f"{path}: more than five conditional directives; split the file before safe formatting")
    for values in itertools.product((0, 1), repeat=len(conditions)):
        if not conditions:
            break
        shadow = bytearray(contents)
        for (keyword, first, last), value in zip(conditions, values):
            if b"\n" in contents[first:last]:
                raise RuntimeError(f"{path}: multiline conditional directive cannot be safely classified")
            replacement = f"#{'elif' if keyword == 'elif' else 'if'} {value}".encode()
            shadow[first:last] = replacement.ljust(last - first, b" ")
        alternate, alternate_file = parser.parse(path, bytes(shadow))
        try:
            scopes += parser.scopes(alternate, alternate_file)
        finally:
            parser.dll.clang_disposeTranslationUnit(alternate)
    edits = {}
    boundaries = [0] + [index + 1 for index, value in enumerate(contents) if value == 10]

    def line(offset):
        return bisect.bisect_right(boundaries, offset) - 1

    def assign(first, last, replacement):
        if contents[first:last].strip():
            raise RuntimeError(f"{path}: attempted to rewrite non-whitespace")
        previous = edits.get((first, last))
        if previous is not None and previous != replacement:
            raise RuntimeError(f"{path}: conditional syntax has conflicting sibling boundaries")
        edits[first, last] = replacement

    for items in scopes:
        merged = []
        for first, last in sorted(set(items)):
            while last > first and contents[last - 1:last].isspace():
                last -= 1
            suffix = last
            while suffix < len(contents) and contents[suffix:suffix + 1] in {b" ", b"\t"}:
                suffix += 1
            if contents[suffix:suffix + 1] in {b";", b","}:
                last = suffix + 1
            if any(start <= first < end or start < last <= end for keyword, start, end in commands):
                continue
            if merged and first < merged[-1][1]:
                merged[-1] = (merged[-1][0], max(last, merged[-1][1]))
            else:
                merged.append((first, last))
        for (previous_first, previous_last), (next_first, next_last) in itertools.pairwise(merged):
            gap = contents[previous_last:next_first]
            if gap.strip():
                continue
            previous_line = line(previous_last - 1)
            next_line = line(next_first)
            if previous_line == next_line:
                raise RuntimeError(f"{path}:{next_line + 1}: multiple sibling items remain on one line")
            multiline = line(previous_first) != previous_line or next_line != line(next_last - 1)
            first = boundaries[previous_line + 1]
            last = boundaries[next_line]
            assign(first, last, b"\n" if multiline else b"")
    includes = [(first, last) for keyword, first, last in commands if keyword == "include"]
    for (previous_first, previous_last), (next_first, next_last) in itertools.pairwise(includes):
        if not contents[previous_last:next_first].strip():
            assign(previous_last, next_first, b"\n")
    for first, last in includes:
        following = next(((text, begin, end) for text, begin, end in tokens if begin >= last), None)
        if following and following[0] != "#" and not contents[last:following[1]].strip():
            start_line = line(following[1])
            assign(last, boundaries[start_line], b"\n\n")
    ordered = sorted(edits.items(), reverse=True)
    previous_start = len(contents) + 1
    result = contents
    for (first, last), replacement in ordered:
        if last > previous_start:
            raise RuntimeError(f"{path}: overlapping spacing edits")
        result = result[:first] + replacement + result[last:]
        previous_start = first
    return result, tuple(text for text, first, last in tokens)


def canonical(parser, path, contents):
    result = subprocess.run(
        [parser.formatter, f"--assume-filename={path}", "--style=file", "--fallback-style=none"],
        input=contents,
        capture_output=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout.replace(b"\r\n", b"\n")


def format_source(parser, path, original):
    wrapped = canonical(parser, path, original)
    formatted, before = spacing(parser, path, wrapped)
    verified, after = spacing(parser, path, canonical(parser, path, formatted))
    if formatted != verified:
        raise RuntimeError(f"{path}: complete formatter pipeline is not idempotent")
    unit, file = parser.parse(path, original)
    try:
        original_tokens = tuple(text for text, first, last in parser.tokens(unit, file, len(original)))
    finally:
        parser.dll.clang_disposeTranslationUnit(unit)
    if before != after or original_tokens != before:
        raise RuntimeError(f"{path}: formatting would alter semantic tokens")
    return formatted


def first_party_path(path):
    path = path.resolve()
    relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else None
    excluded = {"vendor", "generated", "build", "cache", ".cache", "__pycache__"}
    return (
        relative is not None
        and bool(relative.parts)
        and relative.parts[0] in {"src", "tests", "tools"}
        and path.suffix in SUFFIXES
        and not any(part.casefold() in excluded for part in relative.parts)
    )


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--check", action="store_true")
    arguments.add_argument("paths", nargs="*")
    options = arguments.parse_args()
    parser = Parser()
    paths = [Path(value).resolve() for value in options.paths] if options.paths else sorted(
        path for directory in (ROOT / "src", ROOT / "tests", ROOT / "tools")
        for path in directory.rglob("*") if first_party_path(path)
    )
    prepared = []
    for path in paths:
        if not first_party_path(path):
            raise RuntimeError(f"Not a first-party C++ path: {path}")
        original = path.read_bytes()
        formatted = format_source(parser, path, original)
        if formatted != original:
            prepared.append((path, original, formatted))
    if options.check:
        for path, original, formatted in prepared:
            difference = difflib.unified_diff(
                original.decode("utf-8").splitlines(True),
                formatted.decode("utf-8").splitlines(True),
                fromfile=str(path),
                tofile=str(path) + " (formatted)",
            )
            print("".join(difference), end="")
        if prepared:
            print(f"{len(prepared)} file(s) need formatting", file=sys.stderr)
            return 1
    else:
        for path, original, formatted in prepared:
            if path.read_bytes() != original:
                raise RuntimeError(
                    f"{path}: changed during formatting; no files were written, retry after editing stops"
                )
        for path, original, formatted in prepared:
            path.write_bytes(formatted)
            print(f"Formatted {path.relative_to(ROOT)}")
    print(f"Checked {len(paths)} C++ files with clang-format/libclang {VERSION}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"format: {error}", file=sys.stderr)
        sys.exit(2)
