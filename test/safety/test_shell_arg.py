#!/usr/bin/env python3
"""Round-trip the production argument writer through a real POSIX shell."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="shell-arg-") as directory:
    source = Path(directory) / "check.c"
    executable = Path(directory) / "check"
    source.write_text('''#include "util/strings.h"
int main(int argc, char **argv) {
    return argc != 2 || str_write_shell_arg(stdout, argv[1]) || fflush(stdout);
}
''')
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src"), str(source), str(ROOT / "src/util/strings.c"),
                    "-o", str(executable)], check=True)
    values = ["", "192.168.1.1", "Pilot's Wi-Fi", "$(printf unexpected)",
              "`printf unexpected`", "'; printf unexpected; #", "a\nb\r\nc",
              "\\\"$HOME * ? [x] ; & | < > ( )", "-n %s", "réseau"]
    for value in values:
        quoted = subprocess.check_output([str(executable), value])
        result = subprocess.check_output(["/bin/sh", "-c", b"printf '%s' " + quoted], cwd=directory)
        assert result == value.encode(), (value, result)
    print("PASS: shell arguments remain literal (10 cases)")
