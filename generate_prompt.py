#!/usr/bin/env python3
"""Extract specific C++ functions/classes for code review."""

import re
import sys
from pathlib import Path

# ── Configuration: what to extract ──────────────────────────────
# Format: (filepath, [list of patterns])
# Patterns can be:
#   - "ClassName::methodName" → extracts that function definition
#   - "class ClassName" → extracts full class declaration from header
#   - "struct StructName" → extracts full struct declaration
#   - "__FULL__" → dumps the entire file (for small files)


EXTRACTIONS = [
    # Star public API - I got every method name wrong
    ("src/plotting/qcustomplot.cpp", ["QCPGraph::draw", "QCPGraph::drawFill", "QCPGraph::getFillPolygon",
                                      "QCPGraph::getLines", "QCPGraph::getOptimizedLineData", "QCPGraph::drawLinePlot", "QCPGraph::getChannelFillPolygon"]),
    ]


def find_brace_block(text: str, start: int) -> int:
    """Find the closing brace matching the first '{' at or after `start`.
    Returns index after the closing '}'."""
    depth = 0
    i = start
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape_next = False

    while i < len(text):
        c = text[i]

        if escape_next:
            escape_next = False
            i += 1
            continue

        if c == '\\' and (in_string or in_char):
            escape_next = True
            i += 1
            continue

        if in_line_comment:
            if c == '\n':
                in_line_comment = False
            i += 1
            continue

        if in_block_comment:
            if c == '*' and i + 1 < len(text) and text[i + 1] == '/':
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if c == "'":
                in_char = False
            i += 1
            continue

        if c == '/' and i + 1 < len(text):
            if text[i + 1] == '/':
                in_line_comment = True
                i += 2
                continue
            elif text[i + 1] == '*':
                in_block_comment = True
                i += 2
                continue

        if c == '"':
            in_string = True
            i += 1
            continue
        if c == "'":
            in_char = True
            i += 1
            continue

        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1

        i += 1

    return len(text)


def extract_function(text: str, pattern: str) -> str | None:
    """Extract a C++ function definition matching 'Class::method' or similar."""
    # Build regex: return_type pattern(args...) [const] [override] {
    # We search for the pattern as a whole word followed by '('
    escaped = re.escape(pattern)
    # Match optional return type, then the pattern, then parens
    regex = re.compile(
        r'^[^\S\n]*'                    # leading whitespace
        r'(?:[\w\s\*&:<>,]+?\s+)?'      # optional return type (greedy but lazy)
        r'' + escaped + r'\s*\(',        # Class::method(
        re.MULTILINE
    )

    match = regex.search(text)
    if not match:
        # Try without return type (constructors, destructors)
        regex2 = re.compile(
            r'^[^\S\n]*' + escaped + r'\s*\(',
            re.MULTILINE
        )
        match = regex2.search(text)
        if not match:
            return None

    # Walk backwards to grab any preceding comment block
    func_start = match.start()
    lines_before = text[:func_start].split('\n')
    comment_start = func_start
    for line in reversed(lines_before):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*') or stripped == '':
            comment_start -= len(line) + 1  # +1 for newline
        else:
            break
    comment_start = max(0, comment_start)

    # Find the opening brace
    brace_pos = text.find('{', match.end())
    if brace_pos == -1:
        # Might be a declaration only (;)
        semi_pos = text.find(';', match.end())
        if semi_pos != -1:
            return text[comment_start:semi_pos + 1].strip()
        return None

    # Check there's no semicolon before the brace (would mean it's just a declaration)
    between = text[match.end():brace_pos]
    if ';' in between and 'for' not in between and 'if' not in between:
        # It's a forward declaration, not a definition
        return None

    end = find_brace_block(text, brace_pos)
    return text[comment_start:end].strip()


def extract_class_or_struct(text: str, pattern: str) -> str | None:
    """Extract a class or struct declaration from a header file."""
    keyword = "class" if pattern.startswith("class ") else "struct"
    name = pattern.split()[-1]

    # Match: class/struct Name ... {
    regex = re.compile(
        r'^[^\S\n]*' + keyword + r'\s+' + re.escape(name) + r'\b[^;]*?\{',
        re.MULTILINE | re.DOTALL
    )

    match = regex.search(text)
    if not match:
        return None

    # Walk backwards for comments
    start = match.start()
    lines_before = text[:start].split('\n')
    for line in reversed(lines_before):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('*') or stripped.startswith('/*') or stripped == '':
            start -= len(line) + 1
        else:
            break
    start = max(0, start)

    brace_pos = text.find('{', match.start())
    end = find_brace_block(text, brace_pos)

    # Include the semicolon after closing brace
    if end < len(text) and text[end:end + 1] == ';':
        end += 1

    return text[start:end].strip()


def process_file(filepath: str, patterns: list[str], project_root: Path) -> list[str]:
    """Process one file, extracting requested patterns."""
    full_path = project_root / filepath
    results = []

    if not full_path.exists():
        results.append(f"\n// ⚠ FILE NOT FOUND: {filepath}")
        return results

    text = full_path.read_text(encoding='utf-8', errors='replace')

    if "__FULL__" in patterns:
        results.append(f"\n{'='*60}")
        results.append(f"// FILE: {filepath}")
        results.append(f"// (full file, {len(text.splitlines())} lines)")
        results.append(f"{'='*60}")
        results.append(text)
        return results

    for pattern in patterns:
        extracted = None
        if pattern.startswith("class ") or pattern.startswith("struct "):
            extracted = extract_class_or_struct(text, pattern)
        else:
            extracted = extract_function(text, pattern)

        if extracted:
            results.append(f"\n// ── {filepath} :: {pattern} ──")
            results.append(extracted)
        else:
            results.append(f"\n// ⚠ NOT FOUND in {filepath}: {pattern}")

    return results


def main():
    # Try to find project root (look for CMakeLists.txt)
    cwd = Path.cwd()
    project_root = cwd

    # Walk up to find project root
    for p in [cwd] + list(cwd.parents):
        if (p / "CMakeLists.txt").exists() and (p / "src").exists():
            project_root = p
            break

    if len(sys.argv) > 1:
        project_root = Path(sys.argv[1])

    print(f"// Project root: {project_root}")
    print(f"// Extracting specific functions for import wizard review")
    print(f"// {'='*58}")

    missing = []

    for filepath, patterns in EXTRACTIONS:
        chunks = process_file(filepath, patterns, project_root)
        for chunk in chunks:
            print(chunk)
            if "⚠ NOT FOUND" in chunk:
                if "FILE NOT FOUND" in chunk:
                    # Extract just the filepath
                    m = re.search(r'FILE NOT FOUND: (.+)', chunk)
                    if m:
                        missing.append(f"[file missing]  {m.group(1).strip()}")
                else:
                    m = re.search(r'NOT FOUND in (.+?): (.+)', chunk)
                    if m:
                        missing.append(f"{m.group(1).strip()} :: {m.group(2).strip()}")

    print(f"\n// {'='*58}")
    print(f"// Extraction complete.")
    if missing:
        print(f"// {'='*58}")
        print(f"// ⚠ NOT FOUND — {len(missing)} symbol(s) to check manually:")
        for entry in missing:
            print(f"//   • {entry}")
    else:
        print(f"// All requested symbols were found.")
    print(f"// {'='*58}")


if __name__ == "__main__":
    main()