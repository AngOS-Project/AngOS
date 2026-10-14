#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$PWD/kernel/x86_64/entry.c" ]]; then
    PROJECT_ROOT="$PWD"
elif [[ -f "$SCRIPT_DIR/kernel/x86_64/entry.c" ]]; then
    PROJECT_ROOT="$SCRIPT_DIR"
else
    printf '%s\n' "ERROR: Could not find the AngOS project root." "Open UCRT64 in the AngOS project folder, or place this script in the project root." >&2
    exit 1
fi
cd "$PROJECT_ROOT"
python - <<'PYTHON'
from pathlib import Path
from datetime import datetime
import re
import subprocess
import sys

root = Path.cwd()
entry = root / "kernel/x86_64/entry.c"
if not entry.is_file():
    sys.exit("ERROR: kernel/x86_64/entry.c was not found. No files were changed.")
original_bytes = entry.read_bytes()
original = original_bytes.decode("utf-8")
nl = "\r\n" if "\r\n" in original else "\n"
source = original.replace("\r\n", "\n")

if "static void shell_wt(" not in source or 'strcmp_local(cmd, "wt")' not in source:
    sys.exit("ERROR: The Watchtower shell command is not installed in entry.c. Run the Watchtower shell integration script first. No files were changed.")
if 'strcmp_local(topic, "wt")' not in source:
    sys.exit("ERROR: The h wt help section was not found. No files were changed.")


def replace_once(old, new, label, required=True):
    global source
    count = source.count(old)
    if count == 0:
        if new in source:
            return
        if required:
            raise RuntimeError(f"Could not find the expected text for {label}.")
        return
    if count != 1:
        raise RuntimeError(f"Found {count} matches for {label}; expected exactly one.")
    source = source.replace(old, new, 1)

replace_once(
    'terminal_write("Use: h <command> for detailed command help.\\n\\n");',
    'terminal_write("Use: h [command] to list commands or display detailed help.\\n");\n        terminal_write("Notation: <required> must be supplied; [optional] may be omitted.\\n");\n        terminal_write("          Do not type the angle brackets or square brackets.\\n\\n");',
    "general help syntax legend"
)

row_changes = [
    ('terminal_write("h         h <command>            Show detailed command help\\n");', 'terminal_write("h         h [command]             List commands or show detailed help\\n");', "h command-list usage"),
    ('terminal_write("ec        ec <text>              Print text to the terminal\\n");', 'terminal_write("ec        ec [text]               Print text or a blank line\\n");', "ec command-list usage"),
    ('terminal_write("ed        ed <file> <action>     Edit one line of a file\\n");', 'terminal_write("ed        ed <file> <action> [text] Edit one line of a file\\n");', "ed command-list usage"),
    ('terminal_write("cp        cp <source> <dest>     Copy a file\\n");', 'terminal_write("cp        cp <source> <destination> Copy a file to the root directory\\n");', "cp command-list usage"),
]
for old, new, label in row_changes:
    replace_once(old, new, label, required=False)

lines = source.split("\n")
wt_row = '        terminal_write("wt        wt [subcommand]        Inspect diagnostics and event history\\n");'
wt_indexes = [i for i, line in enumerate(lines) if 'terminal_write("wt        wt [subcommand]' in line]
if len(wt_indexes) == 1:
    lines[wt_indexes[0]] = wt_row
elif len(wt_indexes) == 0:
    fat_indexes = [i for i, line in enumerate(lines) if 'terminal_write("fat       fat ' in line]
    if len(fat_indexes) != 1:
        raise SystemExit("ERROR: Could not locate the command-list insertion point before fat. No files were changed.")
    lines.insert(fat_indexes[0], wt_row)
else:
    raise SystemExit("ERROR: Found more than one Watchtower command-list row. No files were changed.")
source = "\n".join(lines)

replace_once(
    'terminal_write("Usage   : h <command>\\n");',
    'terminal_write("Usage   : h [command]\\n");',
    "h detailed-help usage",
    required=False
)
replace_once(
    'terminal_write("Details : With no argument, h lists available commands. With a command\\n");\n        terminal_write("          name, it displays that command\'s purpose, syntax and examples.\\n");',
    'terminal_write("Details : With no argument, h lists the command reference. Supply a\\n");\n        terminal_write("          command name to view its purpose, syntax, subcommands and\\n");\n        terminal_write("          examples. The command reference explains argument notation.\\n");',
    "h detailed-help description",
    required=False
)
replace_once(
    'terminal_write("Usage   : ec <text>\\n");',
    'terminal_write("Usage   : ec [text]\\n");',
    "ec detailed-help usage",
    required=False
)
replace_once(
    'terminal_write("Details : Echo writes to the screen only. It does not create a file\\n");\n        terminal_write("          or save the text to disk.\\n");',
    'terminal_write("Details : With text, ec prints the text to the screen. With no text,\\n");\n        terminal_write("          it prints a blank line. It never creates or saves a file.\\n");',
    "ec detailed-help description",
    required=False
)
replace_once(
    'terminal_write("Usage   : ed <file> <action>\\n");',
    'terminal_write("Usage   : ed <file> <action> [text]\\n");',
    "ed detailed-help usage",
    required=False
)
replace_once(
    'terminal_write("Details : Line numbers start at 1. Ordinary text may be unquoted;\\n");\n        terminal_write("          enclosing text in double quotes is optional.\\n\\n");',
    'terminal_write("Details : The file and action are required. Text is required for +\\n");\n        terminal_write("          and = actions, but must be omitted for - actions. N stands\\n");\n        terminal_write("          for a line number; type the number, not the letter N.\\n");\n        terminal_write("          Ordinary text may be unquoted; double quotes are optional.\\n\\n");',
    "ed detailed-help syntax explanation",
    required=False
)

wt_start = re.search(r'(?m)^[ \t]*\} else if \(strcmp_local\(topic, "wt"\).*?\{[ \t]*$', source)
if not wt_start:
    raise SystemExit("ERROR: Could not identify the start of the h wt help section. No files were changed.")
fat_start = re.search(r'(?m)^[ \t]*\} else if \(strcmp_local\(topic, "fat"\) == 0\) \{', source[wt_start.end():])
if not fat_start:
    raise SystemExit("ERROR: Could not identify the help section following h wt. No files were changed.")
end = wt_start.end() + fat_start.start()
line_start = source.rfind("\n", 0, wt_start.start()) + 1
indent = re.match(r'[ \t]*', source[wt_start.start():]).group(0)
wt_help = '''} else if (strcmp_local(topic, "wt") == 0 || strcmp_local(topic, "watchtower") == 0) {
        terminal_write("Command : wt (AngOS Watchtower)\\n");
        terminal_write("Aliases : watchtower\\n");
        terminal_write("Purpose : Inspect kernel diagnostics and recent recorded events.\\n");
        terminal_write("Usage   : wt [subcommand]\\n");
        terminal_write("Details : With no subcommand, wt displays status. The event history\\n");
        terminal_write("          is held in memory and keeps only the newest 32 events.\\n");
        terminal_write("          Syntax: <required> must be supplied; [optional] may be omitted.\\n\\n");
        terminal_write("Subcommands:\\n");
        terminal_write("  wt status\\n");
        terminal_write("      Show whether diagnostics initialized and how much event-buffer\\n");
        terminal_write("      capacity is currently in use. This does not modify the system.\\n");
        terminal_write("  wt log [count]\\n");
        terminal_write("      Display recent recorded events. Count is an optional positive\\n");
        terminal_write("      decimal number; it defaults to 10 and is capped at 32 events.\\n");
        terminal_write("  wt clear\\n");
        terminal_write("      Clear the in-memory event history. This cannot clear or change\\n");
        terminal_write("      a serial log saved by the host or by QEMU.\\n");
        terminal_write("  wt selftest\\n");
        terminal_write("      Record a test message and verify it can be read back. A passing\\n");
        terminal_write("      test adds an event to the in-memory history.\\n");
        terminal_write("  wt report\\n");
        terminal_write("      Show Watchtower status and the 10 newest events in one report.\\n");
        terminal_write("      This displays the report on screen; it does not save a file.\\n");
        terminal_write("          Fatal panic output is sent to the serial output. Saving it\\n");
        terminal_write("          to a host file depends on the QEMU serial configuration.\\n\\n");
        terminal_write("Examples:\\n");
        terminal_write("  wt\\n");
        terminal_write("  wt status\\n");
        terminal_write("  wt log\\n");
        terminal_write("  wt log 20\\n");
        terminal_write("  wt selftest\\n");
        terminal_write("  wt report\\n");
        terminal_write("  wt clear\\n");

    '''.replace("\n", nl).rstrip() + nl + nl
wt_help = indent + wt_help
source = source[:line_start] + wt_help + source[end:]

if 'wt log [count]' not in source:
    raise SystemExit("ERROR: The expanded Watchtower help text was not inserted correctly. No files were changed.")
if source == original:
    print("The help text is already up to date. No source changes made.")
else:
    updated_bytes = source.replace("\n", nl).encode("utf-8")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = entry.with_name(entry.name + f".before_wt_help_{stamp}.bak")
    backup.write_bytes(original_bytes)
    try:
        entry.write_bytes(updated_bytes)
    except Exception as exc:
        entry.write_bytes(original_bytes)
        sys.exit(f"ERROR: Could not write entry.c: {exc}. Original source restored; backup: {backup.name}")
    print(f"Updated: {entry.relative_to(root)}")
    print(f"Backup: {backup.name}")

build = subprocess.run(["make"])
if build.returncode != 0:
    sys.exit(f"BUILD FAILED: make returned {build.returncode}. Your changes and backup have been preserved.")

deploy = subprocess.run(["make", "deploy"])
if deploy.returncode != 0:
    sys.exit(f"BUILD SUCCEEDED, DEPLOY FAILED: make deploy returned {deploy.returncode}. Your changes and backup have been preserved.")

print("SUCCESS: AngOS built and deployed with the expanded Watchtower help and corrected argument notation.")
print("In AngOS, test: h, h wt, h ed, h ec, wt status, wt log 20, wt selftest, wt report.")
PYTHON
