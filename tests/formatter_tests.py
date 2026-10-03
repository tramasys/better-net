from pathlib import Path
import sys
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from format import Parser, canonical, format_source


class FormatterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.parser = Parser()
        cls.path = ROOT / "tests" / "formatter_fixture.cpp"

    def formatted(self, source):
        result = format_source(self.parser, self.path, source.encode("utf-8"))
        self.assertEqual(result, format_source(self.parser, self.path, result))
        return result.decode("utf-8")

    def test_four_adjacencies_nested_and_wrapped(self):
        source = """
int operation(int, int, int, int);
void fixture()
{
int first = 1;

int second = 2;
if (first)
{
int nested = 1;

++nested;
}
if (second)
{
++second;
}
++first;
const auto long_descriptive_name_for_the_operation_result = operation(first, second, first + second, first + second + 4000);
++second;
}
"""
        output = self.formatted(source)
        self.assertIn("int first = 1;\n    int second = 2;\n\n    if", output)
        self.assertIn("int nested = 1;\n        ++nested;", output)
        self.assertIn("    }\n\n    if (second)", output)
        self.assertIn("    }\n\n    ++first;\n\n    const auto", output)
        self.assertIn("4000);\n\n    ++second;", output)
        self.assertNotEqual(canonical(self.parser, self.path, source.encode()), output.encode())

    def test_lambda_connected_control_and_raw_string(self):
        output = self.formatted('''
void fixture()
{
int value = 0;
const auto callback = [&value](int input)
{
int copy = input;
++copy;
if (copy)
{
value = copy;
}
return value;
};
callback(1);
if (value)
{
++value;
}
else
{
--value;
}
try
{
callback(2);
}
catch (...)
{
value = 0;
}
do
{
--value;
}
while (value > 0);
const auto text = R"sample(raw
#if not_a_directive
  literal whitespace
)sample";
(void)text;
}
''')
        self.assertIn("int value = 0;\n\n    const auto callback", output)
        self.assertIn("    };\n\n    callback(1);\n\n    if", output)
        self.assertIn("    }\n    else\n", output)
        self.assertIn("    }\n    catch (...)\n", output)
        self.assertIn("    } while (value > 0);", output)
        self.assertIn("#if not_a_directive\n  literal whitespace\n", output)
        self.assertNotIn("{\n\n", output)

    def test_preprocessor_both_branches_and_macro(self):
        output = self.formatted('''
#include <cstddef>

#include <cstdint>
#define INCREASE(item) \\
    ++item
void fixture()
{
int value = 0;
#if defined(FORMATTER_BRANCH)
int branch = 1;

++branch;
if (branch)
{
++value;
}
++value;
#else
int alternative = 2;

++alternative;
if (alternative)
{
++value;
}
++value;
#endif
INCREASE(value);
(void)value;
}
int after_conditional_function;
''')
        self.assertIn("#include <cstddef>\n#include <cstdint>\n", output)
        self.assertIn("int branch = 1;\n    ++branch;\n\n    if (branch)", output)
        self.assertIn("int alternative = 2;\n    ++alternative;\n\n    if (alternative)", output)
        self.assertIn("#else", output)
        self.assertIn("#endif\n    INCREASE(value);\n    (void)value;", output)
        self.assertIn("}\n\nint after_conditional_function;", output)

    def test_invalid_syntax_fails_visibly(self):
        with self.assertRaisesRegex(RuntimeError, "requires valid C\\+\\+"):
            self.formatted("void fixture() { int value = ; }")

    def test_includes_separated_once(self):
        output = self.formatted("#include <cstddef>\n\n#include <cstdint>\nint value;\nint second;\n")
        self.assertEqual(output, "#include <cstddef>\n#include <cstdint>\n\nint value;\nint second;\n")

    def test_class_template_and_linkage_scopes(self):
        output = self.formatted("""
template <typename Value>
struct Box
{
Value first;

Value second;
Value get()
{
return first;
}
Value third;
};
extern "C"
{
int first();

int second();
}
""")
        self.assertIn("Value first;\n    Value second;\n\n    Value get()", output)
        self.assertIn("    }\n\n    Value third;", output)
        self.assertIn("int first();\n    int second();", output)

    def test_check_is_nonmutating_and_reports_disagreement(self):
        with tempfile.TemporaryDirectory(prefix=".formatter-", dir=ROOT / "tests") as directory:
            path = Path(directory) / "check.cpp"
            original = b"int first;\n\nint second;\n"
            path.write_bytes(original)
            command = [sys.executable, str(ROOT / "tools" / "format.py")]
            checked = subprocess.run(command + ["--check", str(path)], capture_output=True)
            self.assertEqual(checked.returncode, 1, checked.stderr.decode())
            self.assertEqual(path.read_bytes(), original)
            formatted = subprocess.run(command + [str(path)], capture_output=True)
            self.assertEqual(formatted.returncode, 0, formatted.stderr.decode())
            checked = subprocess.run(command + ["--check", str(path)], capture_output=True)
            self.assertEqual(checked.returncode, 0, checked.stderr.decode())

    def test_explicit_build_path_is_rejected(self):
        checked = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "format.py"), str(ROOT / "build" / "generated.cpp")],
            capture_output=True,
        )
        self.assertEqual(checked.returncode, 2)
        self.assertIn(b"Not a first-party C++ path", checked.stderr)


if __name__ == "__main__":
    unittest.main()
