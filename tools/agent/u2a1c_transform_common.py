#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_text(text, encoding="utf-8")


def require_once(text: str, needle: str, label: str) -> None:
    count = text.count(needle)
    if count != 1:
        raise RuntimeError(f"{label}: expected one occurrence of {needle!r}, found {count}")


def insert_after(text: str, needle: str, addition: str, label: str) -> str:
    require_once(text, needle, label)
    return text.replace(needle, needle + addition, 1)


def insert_before(text: str, needle: str, addition: str, label: str) -> str:
    require_once(text, needle, label)
    return text.replace(needle, addition + needle, 1)


def find_balanced_block(
    text: str, start_marker: str, *, include_trailing_semicolon: bool = False
) -> tuple[int, int, str]:
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"marker not found: {start_marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace not found after: {start_marker}")
    depth = 0
    i = brace
    in_string = False
    quote = ""
    escaped = False
    line_comment = False
    block_comment = False
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if ch == "\n":
                line_comment = False
            i += 1
            continue
        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                i += 2
            else:
                i += 1
            continue
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                in_string = False
            i += 1
            continue
        if ch == "/" and nxt == "/":
            line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            block_comment = True
            i += 2
            continue
        if ch in ('"', "'"):
            in_string = True
            quote = ch
            i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                if include_trailing_semicolon:
                    while end < len(text) and text[end] in " \t\r\n":
                        end += 1
                    if end < len(text) and text[end] == ";":
                        end += 1
                return start, end, text[start:end]
        i += 1
    raise RuntimeError(f"unbalanced block after: {start_marker}")
