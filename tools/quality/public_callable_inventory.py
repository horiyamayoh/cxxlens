#!/usr/bin/env python3
"""Deterministic Clang/Doxygen census helpers for the public callable inventory."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from typing import Any

import jsonschema
import yaml


PUBLIC_PREFIX = "include/cxxlens/"
ROOT = pathlib.Path(__file__).resolve().parents[2]
INVENTORY = pathlib.Path("schemas/cxxlens_ng_public_callable_inventory.yaml")
INVENTORY_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_public_callable_inventory.schema.yaml"
)
REPORT_SCHEMA = pathlib.Path(
    "schemas/cxxlens_ng_public_callable_inventory_report.schema.yaml"
)
CALLABLE_KINDS = {
    "FunctionDecl",
    "CXXMethodDecl",
    "CXXConstructorDecl",
    "CXXDestructorDecl",
    "CXXConversionDecl",
}
TEMPLATE_PARAMETER_KINDS = {
    "TemplateTypeParmDecl",
    "NonTypeTemplateParmDecl",
    "TemplateTemplateParmDecl",
}


class CallableInventoryError(ValueError):
    """A fail-closed public callable inventory violation."""


def fail(message: str) -> None:
    raise CallableInventoryError(message)


def load_document(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        fail(f"cannot load {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected mapping: {path}")
    return value


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def semantic_digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def file_digest(path: pathlib.Path) -> str:
    try:
        return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as error:
        fail(f"cannot digest {path}: {error}")


def validate_schema(document: Any, schema: dict[str, Any], label: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(
            schema,
            format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER,
        ).validate(document)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail(f"{label} schema validation failed: {error.message}")


_CPP_PUNCTUATORS = tuple(
    sorted(
        {
            "%:%:",
            "<=>",
            ">>=",
            "<<=",
            "->*",
            "...",
            "::",
            ".*",
            "->",
            "++",
            "--",
            "<<",
            ">>",
            "<=",
            ">=",
            "==",
            "!=",
            "&&",
            "||",
            "*=",
            "/=",
            "%=",
            "+=",
            "-=",
            "&=",
            "^=",
            "|=",
            "##",
            "<:",
            ">:",
            "<%",
            "%>",
            "%:",
        },
        key=len,
        reverse=True,
    )
)


def _cpp_tokens(value: str) -> list[tuple[str, int, int]]:
    """Lex enough C++ preprocessing tokens to canonicalize declaration spelling."""

    rows: list[tuple[str, int, int]] = []
    offset = 0
    size = len(value)
    while offset < size:
        if value[offset].isspace():
            offset += 1
            continue
        if value.startswith("//", offset):
            newline = value.find("\n", offset + 2)
            offset = size if newline < 0 else newline + 1
            continue
        if value.startswith("/*", offset):
            finish = value.find("*/", offset + 2)
            if finish < 0:
                fail("unterminated C++ block comment in callable spelling")
            offset = finish + 2
            continue

        raw_prefix = next(
            (
                prefix
                for prefix in ('u8R"', 'uR"', 'UR"', 'LR"', 'R"')
                if value.startswith(prefix, offset)
            ),
            None,
        )
        if raw_prefix is not None:
            delimiter_start = offset + len(raw_prefix)
            opening = value.find("(", delimiter_start, delimiter_start + 17)
            if opening < 0:
                fail("invalid C++ raw string delimiter in callable spelling")
            delimiter = value[delimiter_start:opening]
            if any(character.isspace() or character in "()\\" for character in delimiter):
                fail("invalid C++ raw string delimiter in callable spelling")
            closing_text = ")" + delimiter + '"'
            closing = value.find(closing_text, opening + 1)
            if closing < 0:
                fail("unterminated C++ raw string in callable spelling")
            finish = closing + len(closing_text)
            while finish < size and (value[finish].isalnum() or value[finish] == "_"):
                finish += 1
            rows.append((value[offset:finish], offset, finish))
            offset = finish
            continue

        ordinary_prefix = next(
            (
                prefix
                for prefix in ('u8"', "u8'", 'u"', "u'", 'U"', "U'", 'L"', "L'", '"', "'")
                if value.startswith(prefix, offset)
            ),
            None,
        )
        if ordinary_prefix is not None:
            quote = ordinary_prefix[-1]
            cursor = offset + len(ordinary_prefix)
            while cursor < size:
                if value[cursor] == "\\":
                    cursor += 2
                    continue
                if value[cursor] == quote:
                    cursor += 1
                    while cursor < size and (
                        value[cursor].isalnum() or value[cursor] == "_"
                    ):
                        cursor += 1
                    rows.append((value[offset:cursor], offset, cursor))
                    offset = cursor
                    break
                cursor += 1
            else:
                fail("unterminated C++ string or character literal in callable spelling")
            continue

        character = value[offset]
        if character.isalpha() or character == "_" or ord(character) >= 128:
            finish = offset + 1
            while finish < size and (
                value[finish].isalnum() or value[finish] == "_" or ord(value[finish]) >= 128
            ):
                finish += 1
            rows.append((value[offset:finish], offset, finish))
            offset = finish
            continue
        if character.isdigit() or (
            character == "." and offset + 1 < size and value[offset + 1].isdigit()
        ):
            finish = offset + 1
            while finish < size:
                current = value[finish]
                if current.isalnum() or current in "_.'":
                    finish += 1
                elif current in "+-" and value[finish - 1] in "eEpP":
                    finish += 1
                else:
                    break
            rows.append((value[offset:finish], offset, finish))
            offset = finish
            continue
        punctuator = next(
            (item for item in _CPP_PUNCTUATORS if value.startswith(item, offset)),
            character,
        )
        finish = offset + len(punctuator)
        rows.append((punctuator, offset, finish))
        offset = finish
    return rows


def normalize_cpp_text(value: str) -> str:
    """Canonicalize token separation while preserving literal token spelling exactly."""

    return " ".join(token for token, _start, _finish in _cpp_tokens(value))


def admitted_public_headers(root: pathlib.Path) -> list[str]:
    catalog = load_document(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    headers = {
        header
        for collection in (catalog["packages"], catalog["entries"])
        for row in collection
        for header in row["headers"]
    }
    actual = {
        path.relative_to(root).as_posix()
        for path in (root / "include/cxxlens").rglob("*.hpp")
    }
    if headers != actual:
        fail(
            "catalog-driven public header inventory differs: "
            f"missing={sorted(headers - actual)}, extra={sorted(actual - headers)}"
        )
    return sorted(headers)


def _parse_concatenated_json(value: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    offset = 0
    documents: list[dict[str, Any]] = []
    while offset < len(value):
        while offset < len(value) and value[offset].isspace():
            offset += 1
        if offset == len(value):
            break
        try:
            document, offset = decoder.raw_decode(value, offset)
        except json.JSONDecodeError as error:
            fail(f"Clang AST JSON is malformed at byte {error.pos}: {error.msg}")
        if not isinstance(document, dict):
            fail("Clang AST JSON root is not a declaration object")
        documents.append(document)
    if not documents:
        fail("Clang AST census produced no cxxlens declaration roots")
    return documents


def clang_ast_roots(
    root: pathlib.Path,
    compiler: str,
    headers: Iterable[str],
) -> tuple[list[dict[str, Any]], str]:
    version = subprocess.run(
        [compiler, "--version"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    if version.returncode != 0:
        fail(f"cannot execute public callable compiler {compiler}: {version.stderr.strip()}")
    identity = version.stdout.splitlines()[0].strip() if version.stdout else ""
    if not re.search(r"\bclang version 22\.", identity, flags=re.IGNORECASE):
        fail(f"public callable census requires exact Clang major 22, got: {identity}")

    include_lines = [
        f"#include <{header.removeprefix('include/')}>" for header in sorted(headers)
    ]
    with tempfile.TemporaryDirectory(prefix="cxxlens-callable-") as temporary:
        translation_unit = pathlib.Path(temporary) / "public_callable_census.cpp"
        translation_unit.write_text("\n".join(include_lines) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [
                compiler,
                "-std=c++23",
                f"-I{root / 'include'}",
                "-fsyntax-only",
                "-Xclang",
                "-ast-dump=json",
                "-Xclang",
                "-ast-dump-filter=cxxlens",
                str(translation_unit),
            ],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
    if completed.returncode != 0:
        fail(
            "Clang public callable AST extraction failed: "
            + (completed.stderr.strip() or f"exit {completed.returncode}")
        )
    return _parse_concatenated_json(completed.stdout), identity


def _node_file(node: dict[str, Any], fallback: str) -> str:
    location = node.get("loc", {})
    if isinstance(location, dict) and isinstance(location.get("file"), str):
        return location["file"]
    begin = node.get("range", {}).get("begin", {})
    if isinstance(begin, dict) and isinstance(begin.get("file"), str):
        return begin["file"]
    return fallback


def _source_bytes(cache: dict[str, bytes], root: pathlib.Path, header: str) -> bytes:
    if header not in cache:
        try:
            cache[header] = (root / header).read_bytes()
        except OSError as error:
            fail(f"cannot read public header {header}: {error}")
    return cache[header]


def _source_range(
    node: dict[str, Any],
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> str:
    source_range = node.get("range", {})
    begin = source_range.get("begin", {})
    end = source_range.get("end", {})
    if not all(isinstance(value, int) for value in (begin.get("offset"), end.get("offset"))):
        return ""
    start = begin["offset"]
    finish = end["offset"] + int(end.get("tokLen", 0))
    source = _source_bytes(cache, root, header)
    if start < 0 or finish < start or finish > len(source):
        fail(f"invalid Clang source range in {header}: {start}:{finish}")
    return source[start:finish].decode("utf-8")


def _source_offsets(node: dict[str, Any]) -> tuple[int, int] | None:
    source_range = node.get("range", {})
    begin = source_range.get("begin", {})
    end = source_range.get("end", {})
    if not all(
        isinstance(value, int) for value in (begin.get("offset"), end.get("offset"))
    ):
        return None
    return begin["offset"], end["offset"] + int(end.get("tokLen", 0))


def _source_prefix(
    node: dict[str, Any],
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> str:
    begin = node.get("range", {}).get("begin", {})
    location = node.get("loc", {})
    if not all(isinstance(value, int) for value in (begin.get("offset"), location.get("offset"))):
        fail(
            "public callable lacks an exact file-spelled source prefix "
            f"(macro expansion is unsupported): {header}:{node.get('name', '')}"
        )
    start = begin["offset"]
    finish = location["offset"]
    source = _source_bytes(cache, root, header)
    if start < 0 or finish < start or finish > len(source):
        fail(f"invalid callable prefix range in {header}: {start}:{finish}")
    return normalize_cpp_text(source[start:finish].decode("utf-8"))


def _default_argument(
    parameter: dict[str, Any],
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> str | None:
    if "init" not in parameter:
        return None
    ranged_children = [
        child
        for child in parameter.get("inner", [])
        if isinstance(child, dict)
        and isinstance(child.get("range", {}).get("begin", {}).get("offset"), int)
        and isinstance(child.get("range", {}).get("end", {}).get("offset"), int)
    ]
    if not ranged_children:
        fail(f"default argument lacks a source expression: {header}:{parameter.get('name', '')}")
    expression = normalize_cpp_text(
        _source_range(ranged_children[-1], root, header, cache)
    )
    return re.sub(r"^=\s*", "", expression)


def _function_type_parts(qual_type: str) -> tuple[str, str]:
    angle = 0
    square = 0
    for index, character in enumerate(qual_type):
        if character == "<":
            angle += 1
        elif character == ">" and angle:
            angle -= 1
        elif character == "[":
            square += 1
        elif character == "]" and square:
            square -= 1
        elif character == "(" and angle == 0 and square == 0:
            depth = 1
            cursor = index + 1
            while cursor < len(qual_type) and depth:
                if qual_type[cursor] == "(":
                    depth += 1
                elif qual_type[cursor] == ")":
                    depth -= 1
                cursor += 1
            if depth:
                fail(f"unbalanced Clang function type: {qual_type}")
            return qual_type[:index].strip(), qual_type[cursor:].strip()
    fail(f"Clang declaration lacks a function type: {qual_type}")


def _template_parameter(
    node: dict[str, Any],
    scope: str,
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> dict[str, str]:
    kind = {
        "TemplateTypeParmDecl": "type",
        "NonTypeTemplateParmDecl": "non-type",
        "TemplateTemplateParmDecl": "template",
    }.get(node.get("kind"))
    if kind is None:
        fail(f"unsupported template parameter kind: {node.get('kind')}")
    declaration = normalize_cpp_text(_source_range(node, root, header, cache))
    if not declaration:
        fail(f"template parameter lacks source spelling: {header}")
    return {
        "scope": scope,
        "kind": kind,
        "name": node.get("name", ""),
        "declaration": declaration,
    }


def _specialization_name(node: dict[str, Any]) -> str:
    arguments: list[str] = []
    for child in node.get("inner", []):
        if child.get("kind") != "TemplateArgument":
            continue
        argument_type = child.get("type", {}).get("qualType")
        if isinstance(argument_type, str):
            arguments.append(argument_type)
        elif "value" in child:
            arguments.append(str(child["value"]))
        else:
            fail(f"unsupported explicit class template argument in {node.get('name', '')}")
    if not arguments:
        return str(node.get("name", ""))
    return f"{node.get('name', '')}<{', '.join(arguments)}>"


def _parameter_rows(
    node: dict[str, Any],
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for child in node.get("inner", []):
        if child.get("kind") != "ParmVarDecl":
            continue
        parameter_type = child.get("type", {}).get("qualType")
        if not isinstance(parameter_type, str) or not parameter_type:
            fail(f"callable parameter lacks a canonical type: {header}:{node.get('name', '')}")
        rows.append(
            {
                "type": parameter_type,
                "name": child.get("name", ""),
                "default": _default_argument(child, root, header, cache),
            }
        )
    return rows


def _callable_kind(node: dict[str, Any], templated: bool, in_record: bool) -> str:
    if templated:
        return "function-template"
    kind = node.get("kind")
    name = str(node.get("name", ""))
    if kind == "CXXConstructorDecl":
        return "constructor"
    if kind == "CXXDestructorDecl":
        return "destructor"
    if name.startswith("operator"):
        return "operator"
    if node.get("virtual"):
        return "virtual"
    if node.get("storageClass") == "static":
        return "static"
    return "member" if in_record else "free"


def _declaration_state(node: dict[str, Any]) -> str:
    if node.get("explicitlyDeleted"):
        return "deleted"
    if node.get("explicitlyDefaulted"):
        return "defaulted"
    if node.get("pure"):
        return "pure-virtual"
    return "declared"


def _has_body(node: dict[str, Any]) -> bool:
    return any(
        child.get("kind")
        in {"CompoundStmt", "CXXTryStmt", "CoroutineBodyStmt"}
        for child in node.get("inner", [])
        if isinstance(child, dict)
    )


def _source_suffix(
    node: dict[str, Any], function_qualifiers: str, trailing_constraints: list[dict[str, str]]
) -> str:
    pieces: list[str] = []
    cv, ref = _cv_ref_qualifiers(function_qualifiers)
    if cv in {"const", "const-volatile"}:
        pieces.append("const")
    if cv in {"volatile", "const-volatile"}:
        pieces.append("volatile")
    if ref == "rvalue":
        pieces.append("&&")
    elif ref == "lvalue":
        pieces.append("&")
    noexcept = _noexcept_spelling(function_qualifiers)
    if noexcept != "none":
        pieces.append(noexcept)
    pieces.extend(row["source"] for row in trailing_constraints)
    if any(child.get("kind") == "OverrideAttr" for child in node.get("inner", [])):
        pieces.append("override")
    if any(child.get("kind") == "FinalAttr" for child in node.get("inner", [])):
        pieces.append("final")
    state = _declaration_state(node)
    if state == "deleted":
        pieces.append("= delete")
    elif state == "defaulted":
        pieces.append("= default")
    elif state == "pure-virtual":
        pieces.append("= 0")
    return " ".join(pieces)


def _cv_ref_qualifiers(function_qualifiers: str) -> tuple[str, str]:
    top_level: list[str] = []
    depth = 0
    for token, _start, _finish in _cpp_tokens(function_qualifiers):
        if token in {"(", "[", "{"}:
            depth += 1
            continue
        if token in {")",
            "]",
            "}",
        }:
            depth -= 1
            if depth < 0:
                fail(f"unbalanced Clang function qualifier: {function_qualifiers}")
            continue
        if depth == 0:
            top_level.append(token)
    if depth:
        fail(f"unbalanced Clang function qualifier: {function_qualifiers}")
    is_const = "const" in top_level
    is_volatile = "volatile" in top_level
    cv = (
        "const-volatile"
        if is_const and is_volatile
        else "const"
        if is_const
        else "volatile"
        if is_volatile
        else "none"
    )
    ref = "rvalue" if "&&" in top_level else "lvalue" if "&" in top_level else "none"
    return cv, ref


def _noexcept_spelling(function_qualifiers: str) -> str:
    tokens = _cpp_tokens(function_qualifiers)
    for index, (token, _start, _finish) in enumerate(tokens):
        if token != "noexcept":
            continue
        if index + 1 == len(tokens) or tokens[index + 1][0] != "(":
            return "noexcept"
        depth = 0
        for finish in range(index + 1, len(tokens)):
            current = tokens[finish][0]
            if current == "(":
                depth += 1
            elif current == ")":
                depth -= 1
                if depth == 0:
                    return " ".join(row[0] for row in tokens[index : finish + 1])
        fail(f"unbalanced noexcept qualifier: {function_qualifiers}")
    return "none"


def _requires_clause(
    owner: dict[str, Any],
    child: dict[str, Any],
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> str | None:
    owner_offsets = _source_offsets(owner)
    child_offsets = _source_offsets(child)
    if owner_offsets is None or child_offsets is None:
        return None
    owner_start, _owner_finish = owner_offsets
    child_start, child_finish = child_offsets
    if child_start < owner_start:
        return None
    source = _source_bytes(cache, root, header)
    prefix = source[owner_start:child_start].decode("utf-8")
    prefix_tokens = _cpp_tokens(prefix)
    if prefix_tokens and prefix_tokens[-1][0] == "requires":
        token_start = prefix_tokens[-1][1]
        start = owner_start + len(prefix[:token_start].encode("utf-8"))
    else:
        child_text = source[child_start:child_finish].decode("utf-8")
        child_tokens = _cpp_tokens(child_text)
        if not child_tokens or child_tokens[0][0] != "requires":
            return None
        start = child_start
    if child_finish > len(source):
        fail(f"invalid requires clause range in {header}")
    clause = normalize_cpp_text(source[start:child_finish].decode("utf-8"))
    if not clause.startswith("requires "):
        fail(f"requires clause lost its outer token in {header}: {clause}")
    return clause


def _explicit_requires_constraints(
    owner: dict[str, Any],
    *,
    scope: str,
    position: str,
    root: pathlib.Path,
    header: str,
    cache: dict[str, bytes],
) -> tuple[dict[str, str], ...]:
    rows: list[dict[str, str]] = []
    for child in owner.get("inner", []):
        if not isinstance(child, dict):
            continue
        clause = _requires_clause(owner, child, root, header, cache)
        if clause is not None:
            rows.append({"scope": scope, "position": position, "source": clause})
    if len(rows) > 1:
        fail(f"multiple {position} clauses on one callable in {header}")
    return tuple(rows)


def _template_parameter_constraints(
    templates: tuple[dict[str, str], ...],
) -> list[dict[str, str]]:
    return [
        {
            "scope": row["scope"],
            "position": "template-parameter",
            "source": row["declaration"],
        }
        for row in templates
        if row["kind"] == "type"
        and not re.match(r"^(?:class|typename)(?:\s|$)", row["declaration"])
    ]


class _AstCensus:
    def __init__(self, root: pathlib.Path, admitted: set[str]):
        self.root = root
        self.admitted = admitted
        self.source_cache: dict[str, bytes] = {}
        self.rows: list[dict[str, Any]] = []

    def visit(
        self,
        node: dict[str, Any],
        *,
        header: str = "",
        namespaces: tuple[str, ...] = (),
        records: tuple[str, ...] = (),
        record_public: bool = True,
        enclosing_templates: tuple[dict[str, str], ...] = (),
        enclosing_constraints: tuple[dict[str, str], ...] = (),
    ) -> None:
        header = _node_file(node, header)
        header_path = pathlib.Path(header)
        if header_path.is_absolute():
            try:
                header = header_path.relative_to(self.root).as_posix()
            except ValueError:
                return
        kind = node.get("kind")
        if node.get("isImplicit") and kind not in {"NamespaceDecl"}:
            return

        if kind == "NamespaceDecl":
            name = str(node.get("name", ""))
            next_namespaces = namespaces + ((name,) if name else ())
            if "detail" in next_namespaces:
                return
            for child in node.get("inner", []):
                if isinstance(child, dict):
                    self.visit(
                        child,
                        header=header,
                        namespaces=next_namespaces,
                        records=records,
                        record_public=record_public,
                        enclosing_templates=enclosing_templates,
                        enclosing_constraints=enclosing_constraints,
                    )
            return

        if kind == "ClassTemplateDecl":
            parameters = tuple(
                _template_parameter(
                    child, "enclosing", self.root, header, self.source_cache
                )
                for child in node.get("inner", [])
                if child.get("kind") in TEMPLATE_PARAMETER_KINDS
            )
            leading_constraints = _explicit_requires_constraints(
                node,
                scope="enclosing",
                position="leading-requires",
                root=self.root,
                header=header,
                cache=self.source_cache,
            )
            primary_seen = False
            for child in node.get("inner", []):
                if child.get("kind") == "CXXRecordDecl" and not child.get("isImplicit"):
                    self._visit_record(
                        child,
                        header=header,
                        namespaces=namespaces,
                        records=records,
                        record_public=record_public,
                        enclosing_templates=enclosing_templates + parameters,
                        enclosing_constraints=enclosing_constraints
                        + leading_constraints,
                        record_name=str(node.get("name", ""))
                        + (
                            "<" + ", ".join(row["name"] for row in parameters) + ">"
                            if parameters
                            else ""
                        ),
                    )
                    primary_seen = True
                    continue
                if child.get("kind") == "ClassTemplatePartialSpecializationDecl":
                    self.visit(
                        child,
                        header=header,
                        namespaces=namespaces,
                        records=records,
                        record_public=record_public,
                        enclosing_templates=enclosing_templates,
                        enclosing_constraints=enclosing_constraints,
                    )
            if not primary_seen:
                fail(f"class template lacks a primary record declaration in {header}")
            return

        if kind in {
            "ClassTemplateSpecializationDecl",
            "ClassTemplatePartialSpecializationDecl",
        }:
            parameters = (
                tuple(
                    _template_parameter(
                        child, "enclosing", self.root, header, self.source_cache
                    )
                    for child in node.get("inner", [])
                    if child.get("kind") in TEMPLATE_PARAMETER_KINDS
                )
                if kind == "ClassTemplatePartialSpecializationDecl"
                else ()
            )
            leading_constraints = (
                _explicit_requires_constraints(
                    node,
                    scope="enclosing",
                    position="leading-requires",
                    root=self.root,
                    header=header,
                    cache=self.source_cache,
                )
                if kind == "ClassTemplatePartialSpecializationDecl"
                else ()
            )
            self._visit_record(
                node,
                header=header,
                namespaces=namespaces,
                records=records,
                record_public=record_public,
                enclosing_templates=enclosing_templates + parameters,
                enclosing_constraints=enclosing_constraints + leading_constraints,
                record_name=_specialization_name(node),
            )
            return

        if kind == "CXXRecordDecl":
            self._visit_record(
                node,
                header=header,
                namespaces=namespaces,
                records=records,
                record_public=record_public,
                enclosing_templates=enclosing_templates,
                enclosing_constraints=enclosing_constraints,
                record_name=str(node.get("name", "")),
            )
            return

        if kind == "FunctionTemplateDecl":
            parameters = tuple(
                _template_parameter(child, "callable", self.root, header, self.source_cache)
                for child in node.get("inner", [])
                if child.get("kind") in TEMPLATE_PARAMETER_KINDS
            )
            leading_constraints = _explicit_requires_constraints(
                node,
                scope="callable",
                position="leading-requires",
                root=self.root,
                header=header,
                cache=self.source_cache,
            )
            for child in node.get("inner", []):
                if child.get("kind") in CALLABLE_KINDS and not child.get("isImplicit"):
                    self._emit_callable(
                        child,
                        header,
                        namespaces,
                        records,
                        record_public,
                        enclosing_templates + parameters,
                        enclosing_constraints,
                        leading_constraints,
                        templated=True,
                    )
                    break
            return

        if kind == "FriendDecl":
            if not record_public:
                return
            for child in node.get("inner", []):
                if child.get("kind") == "FunctionTemplateDecl":
                    self.visit(
                        child,
                        header=header,
                        namespaces=namespaces,
                        records=(),
                        record_public=True,
                        enclosing_templates=(),
                        enclosing_constraints=(),
                    )
                elif child.get("kind") in CALLABLE_KINDS:
                    self._emit_callable(
                        child,
                        header,
                        namespaces,
                        (),
                        True,
                        (),
                        (),
                        (),
                        templated=False,
                        friend_redeclaration=True,
                    )
            return

        if kind in CALLABLE_KINDS:
            self._emit_callable(
                node,
                header,
                namespaces,
                records,
                record_public,
                enclosing_templates,
                enclosing_constraints,
                (),
                templated=False,
            )

    def _visit_record(
        self,
        node: dict[str, Any],
        *,
        header: str,
        namespaces: tuple[str, ...],
        records: tuple[str, ...],
        record_public: bool,
        enclosing_templates: tuple[dict[str, str], ...],
        enclosing_constraints: tuple[dict[str, str], ...],
        record_name: str,
    ) -> None:
        if not record_name or not record_public:
            return
        if "detail" in records or record_name == "detail":
            return
        access = (
            "public" if node.get("tagUsed") in {"struct", "union"} else "private"
        )
        next_records = records + (record_name,)
        for child in node.get("inner", []):
            if not isinstance(child, dict) or child.get("isImplicit"):
                continue
            if child.get("kind") == "AccessSpecDecl":
                access = child.get("access", access)
                continue
            if child.get("kind") == "FriendDecl":
                self.visit(
                    child,
                    header=header,
                    namespaces=namespaces,
                    records=next_records,
                    record_public=access == "public",
                    enclosing_templates=enclosing_templates,
                    enclosing_constraints=enclosing_constraints,
                )
                continue
            self.visit(
                child,
                header=header,
                namespaces=namespaces,
                records=next_records,
                record_public=access == "public",
                enclosing_templates=enclosing_templates,
                enclosing_constraints=enclosing_constraints,
            )

    def _emit_callable(
        self,
        node: dict[str, Any],
        header: str,
        namespaces: tuple[str, ...],
        records: tuple[str, ...],
        public: bool,
        templates: tuple[dict[str, str], ...],
        enclosing_constraints: tuple[dict[str, str], ...],
        leading_constraints: tuple[dict[str, str], ...],
        *,
        templated: bool,
        friend_redeclaration: bool = False,
    ) -> None:
        if not public or header not in self.admitted or "detail" in namespaces + records:
            return
        name = str(node.get("name", ""))
        if not name:
            fail(f"unnamed public callable in {header}")
        if node.get("kind") in {"CXXConstructorDecl", "CXXDestructorDecl"}:
            name = re.sub(r"<.*>$", "", name)
        qual_type = node.get("type", {}).get("qualType")
        if not isinstance(qual_type, str):
            fail(f"public callable lacks a Clang function type: {header}:{name}")
        return_type, function_qualifiers = _function_type_parts(qual_type)
        if node.get("kind") in {"CXXConstructorDecl", "CXXDestructorDecl"}:
            return_type = ""
        location = node.get("loc", {})
        line = location.get("line")
        column = location.get("col")
        offset = location.get("offset")
        if not isinstance(line, int) or line < 1:
            if not isinstance(offset, int) or offset < 0:
                fail(f"public callable lacks a source line: {header}:{name}")
            source = _source_bytes(self.source_cache, self.root, header)
            if offset > len(source):
                fail(f"public callable offset exceeds its header: {header}:{name}")
            line = source.count(b"\n", 0, offset) + 1
        if not isinstance(column, int) or column < 1:
            if not isinstance(offset, int) or offset < 0:
                fail(f"public callable lacks a source column: {header}:{name}")
            source = _source_bytes(self.source_cache, self.root, header)
            if offset > len(source):
                fail(f"public callable offset exceeds its header: {header}:{name}")
            line_start = source.rfind(b"\n", 0, offset) + 1
            column = offset - line_start + 1
        fully_qualified_name = "::".join((*namespaces, *records, name))
        parameters = _parameter_rows(node, self.root, header, self.source_cache)
        prefix = _source_prefix(node, self.root, header, self.source_cache)
        trailing_constraints = list(
            _explicit_requires_constraints(
                node,
                scope="callable",
                position="trailing-requires",
                root=self.root,
                header=header,
                cache=self.source_cache,
            )
        )
        suffix = _source_suffix(node, function_qualifiers, trailing_constraints)
        enclosing_template_prefix = " ".join(
            f"template <{row['declaration']}>"
            for row in templates
            if row["scope"] == "enclosing"
        )
        callable_template_prefix = " ".join(
            f"template <{row['declaration']}>"
            for row in templates
            if row["scope"] == "callable"
        )
        enclosing_leading = " ".join(
            row["source"] for row in enclosing_constraints
        )
        callable_leading = " ".join(row["source"] for row in leading_constraints)
        parameter_source = ", ".join(
            " ".join(
                part
                for part in (
                    row["type"],
                    row["name"],
                    None if row["default"] is None else f"= {row['default']}",
                )
                if part
            )
            for row in parameters
        )
        source_signature = normalize_cpp_text(
            " ".join(
                part
                for part in (
                    enclosing_template_prefix,
                    enclosing_leading,
                    callable_template_prefix,
                    callable_leading,
                    prefix,
                    fully_qualified_name,
                    f"({parameter_source})",
                    suffix,
                )
                if part
            )
        )
        cv, ref = _cv_ref_qualifiers(function_qualifiers)
        prefix_tokens = {
            token for token, _start, _finish in _cpp_tokens(prefix)
        }
        qualifiers = {
            "cv": cv,
            "ref": ref,
            "noexcept": _noexcept_spelling(function_qualifiers),
        }
        constraints = [
            *(
                row
                for row in _template_parameter_constraints(templates)
                if row["scope"] == "enclosing"
            ),
            *enclosing_constraints,
            *(
                row
                for row in _template_parameter_constraints(templates)
                if row["scope"] == "callable"
            ),
            *leading_constraints,
            *trailing_constraints,
        ]
        state = _declaration_state(node)
        generated = header.startswith("include/cxxlens/relations/")
        origin = (
            "generated-inline"
            if generated
            else state
            if state != "declared"
            else "inline"
            if _has_body(node)
            else "out-of-line"
        )
        self.rows.append(
            {
                "fully_qualified_name": fully_qualified_name,
                "callable_kind": _callable_kind(node, templated, bool(records)),
                "signature": {
                    "source": source_signature,
                    "return_type": return_type,
                    "parameters": parameters,
                    "template_parameters": list(templates),
                    "constraints": constraints,
                    "specifiers": {
                        "static": node.get("storageClass") == "static",
                        "virtual": bool(node.get("virtual")),
                        "pure_virtual": bool(node.get("pure")),
                        "inline": "inline" in prefix_tokens,
                        "constexpr": "constexpr" in prefix_tokens,
                        "consteval": "consteval" in prefix_tokens,
                        "explicit": "explicit" in prefix_tokens,
                        "override": any(
                            child.get("kind") == "OverrideAttr"
                            for child in node.get("inner", [])
                        ),
                        "final": any(
                            child.get("kind") == "FinalAttr"
                            for child in node.get("inner", [])
                        ),
                    },
                    "qualifiers": qualifiers,
                    "declaration_state": state,
                },
                "declaring_header": header,
                "source_line": line,
                "source_column": column,
                "origin": origin,
                "_friend_redeclaration": friend_redeclaration,
                "_decl_id": str(node.get("id", "")),
                "_previous_decl_id": str(node.get("previousDecl", "")),
                "_mangled_name": str(node.get("mangledName", "")),
            }
        )


def extract_ast_census(
    root: pathlib.Path,
    compiler: str,
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    headers = admitted_public_headers(root)
    ast_roots, _compiler_identity = clang_ast_roots(root, compiler, headers)
    census = _AstCensus(root, set(headers))
    for declaration in ast_roots:
        census.visit(declaration)
    previous_by_id = {
        row["_decl_id"]: row["_previous_decl_id"]
        for row in census.rows
        if row["_decl_id"]
    }

    def semantic_decl_identity(row: dict[str, Any]) -> tuple[str, str]:
        if row["_mangled_name"]:
            return "mangled", row["_mangled_name"]
        declaration_id = row["_decl_id"]
        if not declaration_id:
            fail(
                "Clang public callable lacks semantic declaration identity: "
                f"{row['fully_qualified_name']}"
            )
        seen: set[str] = set()
        while previous_by_id.get(declaration_id):
            if declaration_id in seen:
                fail(f"cyclic Clang previousDecl chain: {declaration_id}")
            seen.add(declaration_id)
            declaration_id = previous_by_id[declaration_id]
        return "declaration", declaration_id

    semantic_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in census.rows:
        semantic_groups.setdefault(semantic_decl_identity(row), []).append(row)
    canonical_rows: list[dict[str, Any]] = []
    for group in semantic_groups.values():
        owner_headers = {row["declaring_header"] for row in group}
        if len(owner_headers) != 1:
            fail(
                "one Clang-semantic public callable is declared by multiple "
                f"installed headers: {sorted(owner_headers)} "
                f"({group[0]['fully_qualified_name']})"
            )
        projections = {
            canonical_json(_ast_projection(row)) for row in group
        }
        if len(projections) != 1:
            fail(
                "one Clang-semantic public callable has drifting public "
                f"redeclarations: {next(iter(owner_headers))} "
                f"({group[0]['fully_qualified_name']})"
            )
        ordered_group = sorted(
            group,
            key=lambda row: (
                row["_friend_redeclaration"],
                row["source_line"],
                row["source_column"],
            ),
        )
        canonical_rows.append(ordered_group[0])
    rows = sorted(
        canonical_rows,
        key=lambda row: (
            row["declaring_header"],
            row["source_line"],
            row["source_column"],
            row["fully_qualified_name"],
            row["signature"]["source"],
        ),
    )
    for row in rows:
        for internal in (
            "_friend_redeclaration",
            "_decl_id",
            "_previous_decl_id",
            "_mangled_name",
        ):
            row.pop(internal, None)
    semantic_owners: dict[str, dict[str, Any]] = {}
    for row in rows:
        key = _semantic_ownership_key(row)
        prior = semantic_owners.get(key)
        if prior is not None:
            fail(
                "one semantic public callable is declared by multiple installed headers: "
                f"{prior['declaring_header']} and {row['declaring_header']} "
                f"({row['fully_qualified_name']})"
            )
        semantic_owners[key] = row
    if not rows:
        fail("Clang AST census found no install public callable")
    return rows, {
        "engine": "clang-ast-json",
        "engine_version": "22",
        "canonicalization": "cxxlens.public-callable-signature.v1",
    }


def _xml_text(element: ET.Element | None) -> str:
    return "" if element is None else normalize_cpp_text("".join(element.itertext()))


def extract_doxygen_census(xml_directory: pathlib.Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        xml_files = sorted(xml_directory.glob("*.xml"))
    except OSError as error:
        fail(f"cannot enumerate Doxygen XML: {error}")
    for xml_file in xml_files:
        try:
            root = ET.parse(xml_file).getroot()
        except (OSError, ET.ParseError) as error:
            fail(f"invalid Doxygen XML {xml_file}: {error}")
        compound = root.find("compounddef")
        if compound is None or compound.get("kind") not in {
            "class",
            "struct",
            "union",
            "namespace",
        }:
            continue
        for member in compound.findall("./sectiondef/memberdef[@kind='function']"):
            if compound.get("kind") != "namespace" and member.get("prot") != "public":
                continue
            qualified_name = _xml_text(member.find("qualifiedname"))
            if "::detail::" in re.sub(r"\s+", "", qualified_name):
                continue
            location = member.find("location")
            header = "" if location is None else (location.get("file") or "")
            line_text = "" if location is None else (location.get("line") or "")
            column_text = "" if location is None else (location.get("column") or "")
            if not header.startswith(PUBLIC_PREFIX) or not line_text.isdigit():
                continue
            if not column_text.isdigit() or int(column_text) < 1:
                fail(
                    "Doxygen public callable lacks an exact source column: "
                    f"{header}:{line_text} {qualified_name}"
                )
            template_parameters = []
            for parameter in member.findall("./templateparamlist/param"):
                template_parameters.append(
                    {
                        "type": _xml_text(parameter.find("type")),
                        "name": _xml_text(parameter.find("declname")),
                        "default": _xml_text(parameter.find("defval")) or None,
                    }
                )
            parameters = []
            for parameter in member.findall("./param"):
                parameters.append(
                    {
                        "type": _xml_text(parameter.find("type")),
                        "name": _xml_text(parameter.find("declname")),
                        "default": _xml_text(parameter.find("defval")) or None,
                    }
                )
            projection = {
                "header": header,
                "qualified_name": qualified_name,
                "definition": _xml_text(member.find("definition")),
                "arguments": _xml_text(member.find("argsstring")),
                "requires_clause": _xml_text(member.find("requiresclause")),
                "template_parameters": template_parameters,
                "parameters": parameters,
                "attributes": {
                    key: member.get(key, "no")
                    for key in (
                        "static",
                        "constexpr",
                        "consteval",
                        "const",
                        "volatile",
                        "explicit",
                        "inline",
                        "noexcept",
                        "refqual",
                        "virt",
                    )
                },
            }
            rows.append(
                {
                    "declaring_header": header,
                    "source_line": int(line_text),
                    "source_column": int(column_text),
                    "fully_qualified_name": qualified_name,
                    "doxygen_key": semantic_digest(projection),
                    "projection": projection,
                }
            )
    rows.sort(
        key=lambda row: (
            row["declaring_header"],
            row["source_line"],
            row["source_column"],
            row["fully_qualified_name"],
            row["doxygen_key"],
        )
    )
    if not rows:
        fail("Doxygen XML census found no install public callable")
    keys = [row["doxygen_key"] for row in rows]
    if len(keys) != len(set(keys)):
        fail("Doxygen XML callable projection is not unique")
    return rows


def _callable_root(row: dict[str, Any], prefix: str) -> str:
    name = row["fully_qualified_name"]
    if not name.startswith(prefix):
        return ""
    return name.removeprefix(prefix).split("::", 1)[0]


def _first_parameter_type(row: dict[str, Any]) -> str:
    parameters = row["signature"]["parameters"]
    return "" if not parameters else parameters[0]["type"]


def catalog_entry_id(row: dict[str, Any]) -> str:
    """Resolve exactly one catalog owner from callable semantics."""

    header = row["declaring_header"]
    direct = {
        "include/cxxlens/sdk/common.hpp": "public.common",
        "include/cxxlens/sdk/incremental.hpp": "public.incremental",
        "include/cxxlens/sdk/claim.hpp": "public.claim-kernel",
        "include/cxxlens/sdk/store.hpp": "public.snapshot-store",
        "include/cxxlens/sdk/testing.hpp": "public.provider-sdk",
        "include/cxxlens/provider/clang22.hpp": "public.native-provider-sdk",
    }
    if header in direct:
        return direct[header]
    if header.startswith("include/cxxlens/relations/"):
        return "public.relation-static"

    name = row["fully_qualified_name"]
    if header == "include/cxxlens/sdk/relation.hpp":
        root = _callable_root(row, "cxxlens::sdk::")
        if root in {"catalog_compile_unit", "project_catalog"}:
            return "public.project-catalog"
        if root in {"dynamic_relation", "relation_registry", "relation_engine"}:
            return "public.relation-dynamic"
        return "public.relation-static"

    if header == "include/cxxlens/sdk/query.hpp":
        if name in {
            "cxxlens::sdk::query::from",
            "cxxlens::sdk::query::col",
        }:
            return "public.relation-static"
        root = _callable_root(row, "cxxlens::sdk::query::")
        if root == "dynamic_query":
            return "public.relation-dynamic"
        runtime_roots = {
            "decode_arguments",
            "execution_budget",
            "execution_checkpoint",
            "cancellation_probe",
            "stop_token_cancellation",
            "execution_request",
            "execution_status",
            "query_contributor_edge",
            "annotated_row",
            "query_unresolved",
            "query_explanation",
            "query_guarantee_fragment",
            "query_summary_guarantee",
            "query_result",
            "result_row_view",
            "result_row_cursor",
            "reference_engine",
        }
        if root in runtime_roots or (
            root == "is_valid"
            and _first_parameter_type(row)
            in {"const execution_checkpoint::phase", "const execution_status"}
        ):
            return "public.query-runtime"
        return "public.logical-query"

    if header == "include/cxxlens/sdk/provider.hpp":
        root = _callable_root(row, "cxxlens::sdk::provider::")
        runtime_roots = {
            "sandbox_policy",
            "builtin_sandbox_policies",
            "resolve_sandbox_policy",
            "sandbox_evidence_digest",
            "sandbox_requirement",
            "sandbox_report",
            "provider_fallback_tuple",
            "provider_fallback_policy",
            "provider_candidate_decision",
            "provider_selection",
            "select_provider",
            "provider_process_port",
            "make_system_provider_process_port",
            "process_execution_report",
            "process_provider_runtime",
            "decode_frame_stream",
            "decode_control_text",
            "decode_columnar_batch_end",
        }
        runtime_enum_types = {
            "const sandbox_assurance",
            "const discovery_source",
            "const fallback_direction",
            "const process_status",
        }
        relation_decoder = root == "decode_column_chunk" and any(
            parameter["type"] == "const relation_descriptor &"
            for parameter in row["signature"]["parameters"]
        )
        if (
            root in runtime_roots
            or relation_decoder
            or (root == "is_valid" and _first_parameter_type(row) in runtime_enum_types)
        ):
            return "public.provider-runtime"
        return "public.provider-sdk"

    if header == "include/cxxlens/sdk/recipe.hpp":
        return (
            "public.recipe-foundation"
            if name.startswith("cxxlens::sdk::")
            else "public.recipe"
        )
    fail(f"no catalog ownership rule for public callable: {header}:{name}")


def _package_id(entry: dict[str, Any]) -> str:
    return (
        "clang22-native-author-sdk"
        if entry["target"] == "cxxlens::clang22_provider_sdk"
        else "author-sdk"
    )


def _catalog_stability(entry: dict[str, Any]) -> str:
    return (
        "clang-major-versioned"
        if entry["target"] == "cxxlens::clang22_provider_sdk"
        else "source-versioned"
    )


def _evidence_rows(entry: dict[str, Any]) -> dict[str, Any]:
    evidence = list(entry["implementation_evidence"])
    tests = [path for path in evidence if path.startswith("tests/")]
    examples = [path for path in evidence if path.startswith("examples/")]
    implementations = [
        path
        for path in evidence
        if not path.startswith(("tests/", "examples/", "docs/"))
    ]
    if not implementations:
        fail(f"catalog entry lacks implementation evidence: {entry['id']}")
    return {
        "catalog_digest": semantic_digest(entry),
        "implementation": implementations[0],
        "test": tests[0] if tests else None,
        "example": examples[0] if examples else None,
    }


CALLABLE_ID_DOMAIN = "cxxlens.public-callable-id.v1"


def _identity_scope_key(row: dict[str, Any]) -> tuple[str, str]:
    return row["fully_qualified_name"], row["callable_kind"]


def callable_id(row: dict[str, Any], overload_slot: int) -> str:
    projection = {
        "domain": CALLABLE_ID_DOMAIN,
        "fully_qualified_name": row["fully_qualified_name"],
        "callable_kind": row["callable_kind"],
        "overload_slot": overload_slot,
    }
    return "cpp.callable." + semantic_digest(projection).removeprefix("sha256:")


def _overload_key(row: dict[str, Any]) -> tuple[Any, ...]:
    signature = row["signature"]
    constraints = tuple(
        (
            constraint.get("scope", ""),
            constraint.get("position", ""),
            constraint.get("source", ""),
        )
        if isinstance(constraint, dict)
        else ("legacy", "legacy", str(constraint))
        for constraint in signature["constraints"]
    )
    return (
        row["declaring_header"],
        row["fully_qualified_name"],
        row["callable_kind"],
        tuple(parameter["type"] for parameter in signature["parameters"]),
        tuple(
            (parameter["scope"], parameter["kind"], parameter["declaration"])
            for parameter in signature["template_parameters"]
        ),
        constraints,
        signature["qualifiers"]["cv"],
        signature["qualifiers"]["ref"],
    )


def _alpha_normalized_text(value: str, names: dict[str, str]) -> str:
    return " ".join(
        names.get(token, token) for token, _start, _finish in _cpp_tokens(value)
    )


def _semantic_ownership_key(row: dict[str, Any]) -> str:
    """Identify one C++ callable independently of its declaring header."""

    signature = row["signature"]
    names: dict[str, str] = {}
    scope_indexes = {"enclosing": 0, "callable": 0}
    for parameter in signature["template_parameters"]:
        name = parameter["name"]
        scope = parameter["scope"]
        if name:
            names[name] = f"__cxxlens_{scope}_{scope_indexes[scope]}"
        scope_indexes[scope] += 1
    projection = {
        "fully_qualified_name": _alpha_normalized_text(
            row["fully_qualified_name"], names
        ),
        "callable_kind": row["callable_kind"],
        "return_type": _alpha_normalized_text(signature["return_type"], names),
        "parameter_types": [
            _alpha_normalized_text(parameter["type"], names)
            for parameter in signature["parameters"]
        ],
        "template_parameters": [
            {
                "scope": parameter["scope"],
                "kind": parameter["kind"],
                "declaration": _alpha_normalized_text(
                    parameter["declaration"], names
                ),
            }
            for parameter in signature["template_parameters"]
        ],
        "constraints": [
            {
                "scope": constraint["scope"],
                "position": constraint["position"],
                "source": _alpha_normalized_text(constraint["source"], names),
            }
            for constraint in signature["constraints"]
        ],
        "cv": signature["qualifiers"]["cv"],
        "ref": signature["qualifiers"]["ref"],
    }
    return semantic_digest(projection)


def _coarse_key(row: dict[str, Any]) -> tuple[Any, ...]:
    signature = row["signature"]
    return (
        row["declaring_header"],
        row["fully_qualified_name"],
        row["callable_kind"],
        tuple(parameter["type"] for parameter in signature["parameters"]),
        tuple(
            (parameter["scope"], parameter["kind"])
            for parameter in signature["template_parameters"]
        ),
    )


def _ast_projection(row: dict[str, Any]) -> dict[str, Any]:
    return {
        key: row[key]
        for key in (
            "fully_qualified_name",
            "callable_kind",
            "signature",
            "declaring_header",
            "origin",
        )
    }


def _inventory_projection(document: dict[str, Any]) -> dict[str, Any]:
    return {
        key: document[key]
        for key in (
            "schema",
            "kind",
            "document_version",
            "maturity",
            "authority",
            "inclusion_policy",
            "extractor",
            "allocator",
            "callables",
        )
    }


def inventory_digest(document: dict[str, Any]) -> str:
    return semantic_digest(_inventory_projection(document))


def _validate_source_anchor_order(rows: list[dict[str, Any]]) -> None:
    groups: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for row in rows:
        anchor = row["evidence"]["source_anchor"]
        groups.setdefault(
            (row["declaring_header"], anchor["ast"]["line"]), []
        ).append(row)
    for source_line, group in groups.items():
        ordered = sorted(
            group,
            key=lambda row: row["evidence"]["source_anchor"]["ast"]["column"],
        )
        doxygen_columns = [
            row["evidence"]["source_anchor"]["doxygen"]["column"]
            for row in ordered
        ]
        if doxygen_columns != sorted(doxygen_columns):
            fail(
                "AST/Doxygen callable source order differs on one source line: "
                f"{source_line}"
            )


def _source_anchor(row: dict[str, Any]) -> tuple[str, int, int]:
    return (
        row["declaring_header"],
        row["source_line"],
        row["source_column"],
    )


def _rows_by_source_line(
    rows: list[dict[str, Any]], label: str
) -> dict[tuple[str, int], list[dict[str, Any]]]:
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    anchors: set[tuple[str, int, int]] = set()
    for row in rows:
        anchor = _source_anchor(row)
        if anchor in anchors:
            fail(f"duplicate {label} public callable source anchor: {anchor}")
        anchors.add(anchor)
        grouped.setdefault(anchor[:2], []).append(row)
    for values in grouped.values():
        values.sort(
            key=lambda row: (
                row["source_column"],
                row["fully_qualified_name"],
                row["doxygen_key"]
                if "doxygen_key" in row
                else row["signature"]["source"],
            )
        )
    return grouped


def _pair_ast_doxygen_rows(
    ast_rows: list[dict[str, Any]], doxygen_rows: list[dict[str, Any]]
) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    """Pair independent source coordinates by declaration order on each line."""

    ast_groups = _rows_by_source_line(ast_rows, "AST")
    doxygen_groups = _rows_by_source_line(doxygen_rows, "Doxygen")
    missing = sorted(set(ast_groups) - set(doxygen_groups))
    extra = sorted(set(doxygen_groups) - set(ast_groups))
    if missing or extra:
        fail(
            "AST/Doxygen public callable source-line sets differ: "
            f"AST_only={missing[:8]}, Doxygen_only={extra[:8]}"
        )
    pairs: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for source_line in sorted(ast_groups):
        ast_group = ast_groups[source_line]
        doxygen_group = doxygen_groups[source_line]
        if len(ast_group) != len(doxygen_group):
            fail(
                "AST/Doxygen public callable counts differ on one source line: "
                f"{source_line}, AST={len(ast_group)}, Doxygen={len(doxygen_group)}"
            )
        pairs.extend(zip(ast_group, doxygen_group, strict=True))
    if len(pairs) != len(ast_rows) or len(pairs) != len(doxygen_rows):
        fail(
            "AST/Doxygen public callable counts differ: "
            f"AST={len(ast_rows)}, Doxygen={len(doxygen_rows)}"
        )
    return pairs


def _exact_identity_key(row: dict[str, Any]) -> str:
    return semantic_digest(
        {
            "fully_qualified_name": row["fully_qualified_name"],
            "callable_kind": row["callable_kind"],
            "signature": row["signature"],
        }
    )


def _canonical_allocator_scopes(
    next_slots: dict[tuple[str, str], int],
) -> list[dict[str, Any]]:
    return [
        {
            "fully_qualified_name": name,
            "callable_kind": kind,
            "next_overload_slot": next_slots[(name, kind)],
        }
        for name, kind in sorted(next_slots)
    ]


def _validate_identity_allocator(document: dict[str, Any]) -> None:
    allocator = document.get("allocator")
    if not isinstance(allocator, dict) or allocator.get("domain") != CALLABLE_ID_DOMAIN:
        fail("public callable identity allocator domain differs")
    scopes = allocator.get("scopes")
    if not isinstance(scopes, list):
        fail("public callable identity allocator scopes are missing")
    scope_rows: dict[tuple[str, str], dict[str, Any]] = {}
    for scope in scopes:
        if not isinstance(scope, dict):
            fail("public callable identity allocator scope is not a mapping")
        key = (scope.get("fully_qualified_name"), scope.get("callable_kind"))
        next_slot = scope.get("next_overload_slot")
        if not all(isinstance(value, str) for value in key) or (
            not isinstance(next_slot, int)
            or isinstance(next_slot, bool)
            or next_slot < 0
        ):
            fail("public callable identity allocator scope is malformed")
        if key in scope_rows:
            fail(f"duplicate public callable identity allocator scope: {key}")
        scope_rows[key] = scope
    if scopes != _canonical_allocator_scopes(
        {key: row["next_overload_slot"] for key, row in scope_rows.items()}
    ):
        fail("public callable identity allocator scopes are not canonical")

    used_slots: dict[tuple[str, str], set[int]] = {}
    for row in document.get("callables", []):
        identity = row.get("identity")
        slot = identity.get("overload_slot") if isinstance(identity, dict) else None
        if not isinstance(slot, int) or isinstance(slot, bool) or slot < 0:
            fail(f"public callable identity is malformed: {row.get('id', '')}")
        scope = _identity_scope_key(row)
        allocator_scope = scope_rows.get(scope)
        if allocator_scope is None:
            fail(f"public callable identity scope is absent: {row.get('id', '')}")
        if slot >= allocator_scope["next_overload_slot"]:
            fail(f"public callable identity exceeds allocator high-water: {row['id']}")
        if slot in used_slots.setdefault(scope, set()):
            fail(f"duplicate public callable overload slot: {scope} slot={slot}")
        used_slots[scope].add(slot)
        expected = callable_id(row, slot)
        if row.get("id") != expected:
            fail(
                "public callable stable ID differs from identity: "
                f"expected={expected}, actual={row.get('id', '')}"
            )


def _existing_identity_state(
    existing: dict[str, Any] | None,
) -> tuple[dict[int, int], dict[tuple[str, str], int]]:
    if existing is None:
        return {}, {}
    old_rows = existing.get("callables", [])
    has_allocator = "allocator" in existing
    rows_with_identity = [row for row in old_rows if "identity" in row]
    if has_allocator or rows_with_identity:
        if not has_allocator or len(rows_with_identity) != len(old_rows):
            fail("public callable identity migration state is incomplete")
        _validate_identity_allocator(existing)
        next_slots = {
            (scope["fully_qualified_name"], scope["callable_kind"]): scope[
                "next_overload_slot"
            ]
            for scope in existing["allocator"]["scopes"]
        }
        return {
            index: row["identity"]["overload_slot"]
            for index, row in enumerate(old_rows)
        }, next_slots

    # One-time bootstrap for the pre-allocator inventory. No legacy sequential
    # ID participates in the new identity; slots are canonical within scope.
    grouped: dict[tuple[str, str], list[tuple[int, dict[str, Any]]]] = {}
    for index, row in enumerate(old_rows):
        grouped.setdefault(_identity_scope_key(row), []).append((index, row))
    old_slots: dict[int, int] = {}
    next_slots: dict[tuple[str, str], int] = {}
    for scope, rows in grouped.items():
        ordered = sorted(rows, key=lambda item: _exact_identity_key(item[1]))
        for slot, (index, _) in enumerate(ordered):
            old_slots[index] = slot
        next_slots[scope] = len(ordered)
    return old_slots, next_slots


def _allocate_identities(
    observed: list[dict[str, Any]], existing: dict[str, Any] | None
) -> tuple[dict[int, int], list[dict[str, Any]]]:
    legacy_bootstrap = (
        existing is not None
        and "allocator" not in existing
        and not any("identity" in row for row in existing.get("callables", []))
    )
    old_rows = (
        []
        if existing is None or legacy_bootstrap
        else existing.get("callables", [])
    )
    old_slots, next_slots = (
        ({}, {})
        if legacy_bootstrap
        else _existing_identity_state(existing)
    )
    old_groups: dict[tuple[str, str], list[int]] = {}
    new_groups: dict[tuple[str, str], list[int]] = {}
    for index, row in enumerate(old_rows):
        old_groups.setdefault(_identity_scope_key(row), []).append(index)
    for index, row in enumerate(observed):
        new_groups.setdefault(_identity_scope_key(row), []).append(index)

    assigned: dict[int, int] = {}
    for scope in sorted(set(next_slots) | set(old_groups) | set(new_groups)):
        old_indexes = old_groups.get(scope, [])
        new_indexes = new_groups.get(scope, [])
        high_water = next_slots.get(scope, 0)
        if old_indexes:
            high_water = max(high_water, max(old_slots[index] for index in old_indexes) + 1)

        old_exact: dict[str, list[int]] = {}
        new_exact: dict[str, list[int]] = {}
        for index in old_indexes:
            old_exact.setdefault(_exact_identity_key(old_rows[index]), []).append(index)
        for index in new_indexes:
            new_exact.setdefault(_exact_identity_key(observed[index]), []).append(index)
        matched_old: set[int] = set()
        matched_new: set[int] = set()
        for key in sorted(set(old_exact) & set(new_exact)):
            if len(old_exact[key]) != 1 or len(new_exact[key]) != 1:
                fail(f"duplicate exact callable identity candidate in scope: {scope}")
            old_index = old_exact[key][0]
            new_index = new_exact[key][0]
            assigned[new_index] = old_slots[old_index]
            matched_old.add(old_index)
            matched_new.add(new_index)

        remaining_old = [index for index in old_indexes if index not in matched_old]
        remaining_new = [index for index in new_indexes if index not in matched_new]
        if remaining_old and remaining_new:
            if len(remaining_old) != 1 or len(remaining_new) != 1:
                fail(
                    "ambiguous public callable overload identity migration: "
                    f"scope={scope}, old={len(remaining_old)}, new={len(remaining_new)}"
                )
            assigned[remaining_new[0]] = old_slots[remaining_old[0]]
            remaining_new = []

        for index in sorted(
            remaining_new, key=lambda item: _exact_identity_key(observed[item])
        ):
            assigned[index] = high_water
            high_water += 1
        next_slots[scope] = high_water

    if len(assigned) != len(observed):
        fail("public callable identity allocator did not assign every callable")
    return assigned, _canonical_allocator_scopes(next_slots)


def validate_stable_id_transition(
    previous: dict[str, Any], current: dict[str, Any]
) -> None:
    previous_rows = previous.get("callables", [])
    previous_has_allocator = "allocator" in previous
    previous_has_identities = any("identity" in row for row in previous_rows)
    if not previous_has_allocator and not previous_has_identities:
        # The single migration from the sequential legacy inventory is an
        # explicit bootstrap. Every subsequent revision must carry history.
        return
    if not previous_has_allocator or not all(
        "identity" in row for row in previous_rows
    ):
        fail("previous public callable identity history is incomplete")

    _validate_identity_allocator(previous)
    _validate_identity_allocator(current)
    previous_scopes = {
        (scope["fully_qualified_name"], scope["callable_kind"]): scope[
            "next_overload_slot"
        ]
        for scope in previous["allocator"]["scopes"]
    }
    current_scopes = {
        (scope["fully_qualified_name"], scope["callable_kind"]): scope[
            "next_overload_slot"
        ]
        for scope in current["allocator"]["scopes"]
    }
    missing_scopes = sorted(set(previous_scopes) - set(current_scopes))
    if missing_scopes:
        fail(f"public callable allocator dropped historical scopes: {missing_scopes}")
    decreased = {
        scope: (previous_scopes[scope], current_scopes[scope])
        for scope in previous_scopes
        if current_scopes[scope] < previous_scopes[scope]
    }
    if decreased:
        fail(f"public callable allocator high-water decreased: {decreased}")

    previous_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    current_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in previous_rows:
        previous_groups.setdefault(_identity_scope_key(row), []).append(row)
    for row in current.get("callables", []):
        current_groups.setdefault(_identity_scope_key(row), []).append(row)

    for scope in sorted(set(previous_groups) | set(current_groups)):
        old_rows = previous_groups.get(scope, [])
        new_rows = current_groups.get(scope, [])
        old_exact: dict[str, list[dict[str, Any]]] = {}
        new_exact: dict[str, list[dict[str, Any]]] = {}
        for row in old_rows:
            old_exact.setdefault(_exact_identity_key(row), []).append(row)
        for row in new_rows:
            new_exact.setdefault(_exact_identity_key(row), []).append(row)
        matched_old: set[str] = set()
        matched_new: set[str] = set()
        for key in sorted(set(old_exact) & set(new_exact)):
            if len(old_exact[key]) != 1 or len(new_exact[key]) != 1:
                fail(f"duplicate exact stable-ID transition candidate: {scope}")
            old_row = old_exact[key][0]
            new_row = new_exact[key][0]
            if old_row["id"] != new_row["id"]:
                fail(
                    "exact public callable changed stable ID: "
                    f"{old_row['id']} -> {new_row['id']}"
                )
            matched_old.add(old_row["id"])
            matched_new.add(new_row["id"])

        remaining_old = [row for row in old_rows if row["id"] not in matched_old]
        remaining_new = [row for row in new_rows if row["id"] not in matched_new]
        if remaining_old and remaining_new:
            if len(remaining_old) != 1 or len(remaining_new) != 1:
                fail(
                    "ambiguous public callable stable-ID transition: "
                    f"scope={scope}, old={len(remaining_old)}, new={len(remaining_new)}"
                )
            if remaining_old[0]["id"] != remaining_new[0]["id"]:
                fail(
                    "changed public callable did not preserve stable ID: "
                    f"{remaining_old[0]['id']} -> {remaining_new[0]['id']}"
                )
            remaining_new = []

        old_high_water = previous_scopes.get(scope, 0)
        reused = [
            row
            for row in remaining_new
            if row["identity"]["overload_slot"] < old_high_water
        ]
        if reused:
            fail(
                "public callable allocator reused a historical overload slot: "
                + ", ".join(row["id"] for row in reused)
            )


def build_inventory(
    root: pathlib.Path,
    compiler: str,
    doxygen_xml: pathlib.Path,
    existing: dict[str, Any] | None = None,
) -> dict[str, Any]:
    observed, extractor = extract_ast_census(root, compiler)
    doxygen_rows = extract_doxygen_census(doxygen_xml)
    paired_rows = _pair_ast_doxygen_rows(observed, doxygen_rows)
    doxygen_by_callable = {
        _overload_key(ast_row): doxygen_row
        for ast_row, doxygen_row in paired_rows
    }

    catalog = load_document(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    entries = {row["id"]: row for row in catalog["entries"]}
    identity_slots, allocator_scopes = _allocate_identities(observed, existing)
    callables: list[dict[str, Any]] = []
    for index, ast_row in enumerate(observed):
        doxygen = doxygen_by_callable.get(_overload_key(ast_row))
        if doxygen is None:
            fail(
                "AST callable is absent from Doxygen XML: "
                f"{ast_row['declaring_header']}:{ast_row['source_line']}:"
                f"{ast_row['source_column']} "
                f"{ast_row['fully_qualified_name']}"
            )
        overload_slot = identity_slots[index]
        identifier = callable_id(ast_row, overload_slot)
        entry_id = catalog_entry_id(ast_row)
        entry = entries.get(entry_id)
        if entry is None:
            fail(f"callable owner is not a catalog entry: {entry_id}")
        evidence = _evidence_rows(entry)
        evidence.update(
            {
                "doxygen_key": doxygen["doxygen_key"],
                "doxygen_name": doxygen["fully_qualified_name"],
                "source_anchor": {
                    "ast": {
                        "line": ast_row["source_line"],
                        "column": ast_row["source_column"],
                    },
                    "doxygen": {
                        "line": doxygen["source_line"],
                        "column": doxygen["source_column"],
                    },
                },
            }
        )
        callables.append(
            {
                "id": identifier,
                "identity": {"overload_slot": overload_slot},
                "catalog_entry": entry_id,
                "package": _package_id(entry),
                "target": entry["target"],
                **_ast_projection(ast_row),
                "status": entry["status"],
                "stability": _catalog_stability(entry),
                "qualification": entry["profile"],
                "owner": entry["owner_issue"],
                "evidence": evidence,
            }
        )
    callables.sort(key=lambda row: row["id"])
    document = {
        "schema": "cxxlens.ng-public-callable-inventory.v1",
        "kind": "public-cpp-callable-inventory",
        "document_version": "1.1.0",
        "maturity": "implemented",
        "authority": {
            "design": "docs/design/cxxlens_next_generation_integrated_design_ja.md",
            "catalog": "schemas/cxxlens_ng_public_api_catalog.yaml",
            "decision_adr": "docs/design/adr/0092-exact-public-callable-inventory.md",
            "owner_issue": "#169",
            "owner": "steward.ng-sdk",
        },
        "inclusion_policy": {
            "include": [
                "installed-public-free-functions-and-function-templates",
                "public-constructors-destructors-members-static-members-operators-and-virtuals",
                "explicitly-deleted-and-explicitly-defaulted-public-callables",
                "inline-constexpr-consteval-and-generated-relation-tag-callables",
            ],
            "exclude": [
                "private-protected-and-detail-callables",
                "non-installed-headers",
                "compiler-implicit-callables",
            ],
            "ownership": "exactly-one-catalog-entry-per-callable",
        },
        "extractor": extractor,
        "allocator": {
            "domain": CALLABLE_ID_DOMAIN,
            "scopes": allocator_scopes,
        },
        "inventory_digest": "",
        "callables": callables,
    }
    document["inventory_digest"] = inventory_digest(document)
    return document


def validate_inventory_document(root: pathlib.Path, document: dict[str, Any]) -> None:
    validate_schema(
        document,
        load_document(root / INVENTORY_SCHEMA),
        "public callable inventory",
    )
    if document["inventory_digest"] != inventory_digest(document):
        fail("public callable inventory semantic digest differs")
    _validate_identity_allocator(document)
    rows = document["callables"]
    if rows != sorted(rows, key=lambda row: row["id"]):
        fail("public callable inventory rows are not ordered by stable ID")
    identifiers = [row["id"] for row in rows]
    if len(identifiers) != len(set(identifiers)):
        fail("public callable inventory has duplicate stable IDs")
    overload_keys = [_overload_key(row) for row in rows]
    if len(overload_keys) != len(set(overload_keys)):
        fail("one public callable is owned by multiple inventory rows")
    semantic_keys = [_semantic_ownership_key(row) for row in rows]
    if len(semantic_keys) != len(set(semantic_keys)):
        fail("one semantic public callable is owned by multiple inventory rows")
    doxygen_keys = [row["evidence"]["doxygen_key"] for row in rows]
    if len(doxygen_keys) != len(set(doxygen_keys)):
        fail("public callable inventory has duplicate Doxygen ownership")
    ast_anchors = [
        (
            row["declaring_header"],
            row["evidence"]["source_anchor"]["ast"]["line"],
            row["evidence"]["source_anchor"]["ast"]["column"],
        )
        for row in rows
    ]
    doxygen_anchors = [
        (
            row["declaring_header"],
            row["evidence"]["source_anchor"]["doxygen"]["line"],
            row["evidence"]["source_anchor"]["doxygen"]["column"],
        )
        for row in rows
    ]
    if len(ast_anchors) != len(set(ast_anchors)):
        fail("public callable inventory has duplicate AST source anchors")
    if len(doxygen_anchors) != len(set(doxygen_anchors)):
        fail("public callable inventory has duplicate Doxygen source anchors")
    _validate_source_anchor_order(rows)
    for row in rows:
        source_anchor = row["evidence"]["source_anchor"]
        if source_anchor["ast"]["line"] != source_anchor["doxygen"]["line"]:
            fail(f"AST/Doxygen callable source lines differ: {row['id']}")

    catalog = load_document(root / "schemas/cxxlens_ng_public_api_catalog.yaml")
    entries = {row["id"]: row for row in catalog["entries"]}
    packages = {row["id"]: row for row in catalog["packages"]}
    admitted = set(admitted_public_headers(root))
    for row in rows:
        header = row["declaring_header"]
        if header not in admitted:
            fail(f"inventory row names a non-admitted header: {row['id']} {header}")
        expected_entry_id = catalog_entry_id(row)
        if row["catalog_entry"] != expected_entry_id:
            fail(
                f"callable catalog ownership differs: {row['id']} "
                f"expected={expected_entry_id}, actual={row['catalog_entry']}"
            )
        entry = entries[expected_entry_id]
        if header not in entry["headers"]:
            fail(f"catalog entry does not admit callable header: {row['id']}")
        expected_package = _package_id(entry)
        if row["package"] != expected_package or expected_package not in packages:
            fail(f"callable package ownership differs: {row['id']}")
        expected = {
            "target": entry["target"],
            "status": entry["status"],
            "stability": _catalog_stability(entry),
            "qualification": entry["profile"],
            "owner": entry["owner_issue"],
        }
        for key, value in expected.items():
            if row[key] != value:
                fail(f"callable {key} differs from catalog: {row['id']}")
        expected_evidence = _evidence_rows(entry)
        for key, value in expected_evidence.items():
            if row["evidence"][key] != value:
                fail(f"callable catalog evidence differs: {row['id']} {key}")
        for key in ("implementation", "test", "example"):
            path = row["evidence"][key]
            if path is not None and not (root / path).is_file():
                fail(f"callable evidence path is missing: {row['id']} {path}")
        ast_name = re.sub(r"\s+", "", row["fully_qualified_name"])
        doxygen_name = re.sub(r"\s+", "", row["evidence"]["doxygen_name"])
        if ast_name != doxygen_name and re.sub(
            r"<[^<>]*>", "", ast_name
        ) != re.sub(r"<[^<>]*>", "", doxygen_name):
            fail(f"callable Doxygen name differs: {row['id']}")
        if header.startswith("include/cxxlens/relations/") != (
            row["origin"] == "generated-inline"
        ):
            fail(f"generated callable origin differs: {row['id']}")


def check_ast_inventory(
    root: pathlib.Path,
    compiler: str,
    document: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    observed, extractor = extract_ast_census(root, compiler)
    if extractor != document["extractor"]:
        fail(
            "public callable extractor identity differs: "
            f"expected={document['extractor']}, actual={extractor}"
        )
    expected_by_key = {_overload_key(row): row for row in document["callables"]}
    observed_by_key = {_overload_key(row): row for row in observed}
    missing = sorted(set(expected_by_key) - set(observed_by_key), key=str)
    extra = sorted(set(observed_by_key) - set(expected_by_key), key=str)
    if missing or extra:
        fail(
            "header/inventory callable sets differ: "
            f"inventory_only={missing[:8]}, header_only={extra[:8]}"
        )
    mismatches = []
    for key in sorted(expected_by_key, key=str):
        expected = _ast_projection(expected_by_key[key])
        actual = _ast_projection(observed_by_key[key])
        expected_anchor = expected_by_key[key]["evidence"]["source_anchor"]["ast"]
        actual_anchor = {
            "line": observed_by_key[key]["source_line"],
            "column": observed_by_key[key]["source_column"],
        }
        if expected != actual or expected_anchor != actual_anchor:
            mismatches.append(
                {
                    "id": expected_by_key[key]["id"],
                    "expected": expected,
                    "actual": actual,
                    "expected_anchor": expected_anchor,
                    "actual_anchor": actual_anchor,
                }
            )
    if mismatches:
        fail(f"public callable signatures differ: {mismatches[:4]}")
    return observed, extractor


def check_doxygen_inventory(
    document: dict[str, Any], xml_directory: pathlib.Path
) -> list[dict[str, Any]]:
    observed = extract_doxygen_census(xml_directory)
    expected_by_anchor = {
        (
            row["declaring_header"],
            row["evidence"]["source_anchor"]["doxygen"]["line"],
            row["evidence"]["source_anchor"]["doxygen"]["column"],
        ): row
        for row in document["callables"]
    }
    actual_by_anchor = {_source_anchor(row): row for row in observed}
    missing = sorted(set(expected_by_anchor) - set(actual_by_anchor))
    extra = sorted(set(actual_by_anchor) - set(expected_by_anchor))
    if missing or extra:
        fail(
            "Doxygen/inventory callable source anchors differ: "
            f"inventory_only={missing[:8]}, doxygen_only={extra[:8]}"
        )
    mismatches = []
    for anchor in sorted(expected_by_anchor):
        expected = expected_by_anchor[anchor]
        actual = actual_by_anchor[anchor]
        if (
            expected["evidence"]["doxygen_key"] != actual["doxygen_key"]
            or expected["evidence"]["doxygen_name"]
            != actual["fully_qualified_name"]
        ):
            mismatches.append(
                {
                    "id": expected["id"],
                    "anchor": anchor,
                    "expected_key": expected["evidence"]["doxygen_key"],
                    "actual_key": actual["doxygen_key"],
                    "expected_name": expected["evidence"]["doxygen_name"],
                    "actual_name": actual["fully_qualified_name"],
                }
            )
    if mismatches:
        fail(f"Doxygen/inventory callable projections differ: {mismatches[:4]}")
    if len(observed) != len(document["callables"]):
        fail(
            "Doxygen/inventory callable counts differ: "
            f"inventory={len(document['callables'])}, Doxygen={len(observed)}"
        )
    return observed


def git_output(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def _git_show_inventory(
    root: pathlib.Path, revision: str, relative: pathlib.PurePath
) -> tuple[str, dict[str, Any]] | None:
    completed = subprocess.run(
        ["git", "-C", str(root), "show", f"{revision}:{relative.as_posix()}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        tree = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "ls-tree",
                "--name-only",
                revision,
                "--",
                relative.as_posix(),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if tree.returncode != 0:
            fail(
                "cannot inspect historical public callable inventory at "
                f"{revision}: {tree.stderr.strip()}"
            )
        if not tree.stdout.strip():
            return None
        fail(
            "cannot read historical public callable inventory at "
            f"{revision}: {completed.stderr.strip()}"
        )
    try:
        value = yaml.safe_load(completed.stdout)
    except yaml.YAMLError as error:
        fail(f"cannot parse historical public callable inventory: {error}")
    if not isinstance(value, dict):
        fail("historical public callable inventory is not a mapping")
    return completed.stdout, value


def _git_revision_exists(root: pathlib.Path, revision: str) -> bool:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "--quiet", revision],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode == 0:
        return True
    if completed.stderr.strip():
        fail(f"cannot inspect git revision {revision}: {completed.stderr.strip()}")
    return False


def previous_inventory_for_check(
    root: pathlib.Path,
    inventory_path: pathlib.Path,
) -> dict[str, Any] | None:
    try:
        relative = inventory_path.resolve().relative_to(root.resolve())
    except ValueError:
        return None
    head = _git_show_inventory(root, "HEAD", relative)
    if head is None:
        return None
    head_text, head_document = head
    if inventory_path.read_text(encoding="utf-8") != head_text:
        return head_document
    if not _git_revision_exists(root, "HEAD^"):
        if git_output(root, "rev-parse", "--is-shallow-repository") == "true":
            fail(
                "stable public callable ID history is unavailable in a shallow "
                "checkout; fetch the parent revision"
            )
        return None
    parent = _git_show_inventory(root, "HEAD^", relative)
    return None if parent is None else parent[1]


def current_git_state(root: pathlib.Path) -> dict[str, Any]:
    return {
        "revision": git_output(root, "rev-parse", "HEAD"),
        "tree": git_output(root, "rev-parse", "HEAD^{tree}"),
        "branch": git_output(root, "branch", "--show-current"),
        "clean": git_output(root, "status", "--porcelain=v1") == "",
    }


def _markdown_escape(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def doxygen_correspondence_digest(rows: list[dict[str, Any]]) -> str:
    return semantic_digest([row["projection"] for row in rows])


def review_markdown(
    document: dict[str, Any],
    git: dict[str, Any],
    run_url: str,
    doxygen_digest: str,
) -> str:
    extractor = document["extractor"]
    lines = [
        "# Exact public callable inventory review",
        "",
        f"- Revision: `{git['revision']}`",
        f"- Tree: `{git['tree']}`",
        f"- Inventory digest: `{document['inventory_digest']}`",
        f"- Callable count: `{len(document['callables'])}`",
        f"- Extractor: `{extractor['engine']} {extractor['engine_version']}`",
        f"- Doxygen correspondence digest: `{doxygen_digest}`",
        f"- CI run: {run_url}",
        "",
        "| ID | Catalog entry | Target | Header | Fully-qualified signature | Status | Stability | Qualification | Owner |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in document["callables"]:
        values = (
            row["id"],
            row["catalog_entry"],
            row["target"],
            row["declaring_header"],
            row["signature"]["source"],
            row["status"],
            row["stability"],
            row["qualification"],
            row["owner"],
        )
        lines.append("| " + " | ".join(_markdown_escape(value) for value in values) + " |")
    return "\n".join(lines) + "\n"


def _require_canonical_report_inventory(
    root: pathlib.Path,
    inventory_path: pathlib.Path,
    document: dict[str, Any],
) -> pathlib.Path:
    canonical_path = (root / INVENTORY).resolve()
    if inventory_path.resolve() != canonical_path:
        fail(
            "public callable review requires the canonical inventory path: "
            f"{canonical_path}"
        )
    canonical_document = load_document(canonical_path)
    if canonical_json(document) != canonical_json(canonical_document):
        fail("public callable review document differs from the canonical inventory")
    return canonical_path


def build_report(
    root: pathlib.Path,
    document: dict[str, Any],
    doxygen_rows: list[dict[str, Any]],
    inventory_path: pathlib.Path,
    markdown_path: pathlib.Path,
    generated_at: str,
    run_url: str,
    expected_revision: str,
) -> dict[str, Any]:
    inventory_path = _require_canonical_report_inventory(
        root, inventory_path, document
    )
    git = current_git_state(root)
    if git["revision"] != expected_revision or git["branch"] != "main" or not git["clean"]:
        fail(f"callable inventory review requires exact clean main revision: {git}")
    headers = admitted_public_headers(root)
    return {
        "schema": "cxxlens.ng-public-callable-inventory-report.v1",
        "result": "passed",
        "generated_at": generated_at,
        "run_url": run_url,
        "git": git,
        "inventory": {
            "path": INVENTORY.as_posix(),
            "file_digest": file_digest(inventory_path),
            "semantic_digest": document["inventory_digest"],
            "callable_count": len(document["callables"]),
        },
        "extractor": document["extractor"],
        "headers": {
            "count": len(headers),
            "digest": semantic_digest(headers),
        },
        "doxygen": {
            "count": len(doxygen_rows),
            "digest": doxygen_correspondence_digest(doxygen_rows),
        },
        "review": {
            "path": markdown_path.name,
            "digest": file_digest(markdown_path),
        },
    }


def _write_yaml(path: pathlib.Path, document: dict[str, Any]) -> None:
    path.write_text(
        yaml.safe_dump(
            document,
            allow_unicode=True,
            sort_keys=False,
            width=120,
        ),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command", choices=("check", "generate", "check-doxygen", "report")
    )
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--compiler", default="clang++-22")
    parser.add_argument("--inventory", type=pathlib.Path, default=INVENTORY)
    parser.add_argument("--doxygen-xml", type=pathlib.Path)
    parser.add_argument("--output-json", type=pathlib.Path)
    parser.add_argument("--output-markdown", type=pathlib.Path)
    parser.add_argument("--run-url")
    parser.add_argument("--expected-revision")
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    inventory_path = (
        arguments.inventory
        if arguments.inventory.is_absolute()
        else root / arguments.inventory
    )
    try:
        existing = load_document(inventory_path) if inventory_path.is_file() else None
        if arguments.command == "generate":
            if arguments.doxygen_xml is None:
                fail("generate requires --doxygen-xml")
            document = build_inventory(
                root,
                arguments.compiler,
                arguments.doxygen_xml.resolve(),
                existing,
            )
            validate_inventory_document(root, document)
            if existing is not None:
                validate_stable_id_transition(existing, document)
            _write_yaml(inventory_path, document)
            print(
                f"wrote {len(document['callables'])} public callables to {inventory_path}"
            )
            return 0
        if existing is None:
            fail(f"public callable inventory is missing: {inventory_path}")
        validate_inventory_document(root, existing)
        if arguments.command == "report":
            inventory_path = _require_canonical_report_inventory(
                root, inventory_path, existing
            )
        if arguments.command == "check":
            previous = previous_inventory_for_check(root, inventory_path)
            if previous is not None:
                validate_stable_id_transition(previous, existing)
            check_ast_inventory(root, arguments.compiler, existing)
            print(
                f"public callable inventory check passed ({len(existing['callables'])} callables)"
            )
            return 0
        if arguments.doxygen_xml is None:
            fail(f"{arguments.command} requires --doxygen-xml")
        doxygen_rows = check_doxygen_inventory(
            existing, arguments.doxygen_xml.resolve()
        )
        if arguments.command == "check-doxygen":
            print(
                f"Doxygen callable inventory check passed ({len(doxygen_rows)} callables)"
            )
            return 0
        if not all(
            (
                arguments.output_json,
                arguments.output_markdown,
                arguments.run_url,
                arguments.expected_revision,
            )
        ):
            fail(
                "report requires --output-json, --output-markdown, --run-url, "
                "and --expected-revision"
            )
        check_ast_inventory(root, arguments.compiler, existing)
        generated_at = (
            datetime.datetime.now(datetime.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z")
        )
        git = current_git_state(root)
        doxygen_digest = doxygen_correspondence_digest(doxygen_rows)
        markdown = review_markdown(
            existing, git, arguments.run_url, doxygen_digest
        )
        arguments.output_markdown.write_text(markdown, encoding="utf-8")
        report = build_report(
            root,
            existing,
            doxygen_rows,
            inventory_path,
            arguments.output_markdown,
            generated_at,
            arguments.run_url,
            arguments.expected_revision,
        )
        validate_schema(
            report,
            load_document(root / REPORT_SCHEMA),
            "public callable inventory report",
        )
        arguments.output_json.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"wrote public callable review report to {arguments.output_json}")
    except (
        CallableInventoryError,
        OSError,
        json.JSONDecodeError,
        yaml.YAMLError,
        ET.ParseError,
    ) as error:
        print(f"public callable inventory check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
