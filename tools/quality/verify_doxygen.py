#!/usr/bin/env python3
"""Validate generated Doxygen XML against the public API catalog."""

from __future__ import annotations

import argparse
import collections
import pathlib
import sys
import xml.etree.ElementTree as ET

import yaml


def text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext()).strip()


def catalog_headers(catalog_path: pathlib.Path) -> set[str]:
    catalog = yaml.safe_load(catalog_path.read_text(encoding="utf-8"))
    return {
        header
        for entry in catalog.get("entries", [])
        if entry.get("status") == "implemented"
        for header in entry.get("headers", [])
    }


def catalog_value_headers(catalog_path: pathlib.Path) -> dict[str, set[str]]:
    catalog = yaml.safe_load(catalog_path.read_text(encoding="utf-8"))
    kinds: dict[str, set[str]] = collections.defaultdict(set)
    names: dict[str, set[str]] = collections.defaultdict(set)
    for entry in catalog.get("entries", []):
        if entry.get("status") != "implemented":
            continue
        for header in entry.get("headers", []):
            for symbol in entry.get("symbols", []):
                kinds[header].add(symbol.get("kind", ""))
                names[header].add(symbol.get("name", "").rsplit(".", 1)[-1])
    return {header: names[header] for header in kinds if kinds[header] == {"value"}}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xml_directory", type=pathlib.Path)
    parser.add_argument("--ng-catalog", type=pathlib.Path, required=True)
    args = parser.parse_args()

    failures: list[str] = []
    checked = 0
    ng_headers = catalog_headers(args.ng_catalog)
    value_headers = catalog_value_headers(args.ng_catalog)
    observed_ng_headers: collections.Counter[str] = collections.Counter()
    if not ng_headers:
        failures.append("catalog has no implemented public headers")

    for xml_file in sorted(args.xml_directory.glob("*.xml")):
        root = ET.parse(xml_file).getroot()
        compound = root.find("compounddef")
        if compound is None or compound.get("kind") not in {"class", "struct", "namespace"}:
            continue
        if compound.get("kind") in {"class", "struct"} and compound.get("prot") == "public":
            location = compound.find("location")
            location_file = "" if location is None else (location.get("file") or "")
            name = text(compound.find("compoundname")).rsplit("::", 1)[-1]
            documented = text(compound.find("briefdescription")) or text(
                compound.find("detaileddescription")
            )
            for header, names in value_headers.items():
                if location_file.endswith(header) and name in names and documented:
                    observed_ng_headers[header] += 1
        for member in compound.findall("./sectiondef/memberdef[@kind='function']"):
            if compound.get("kind") != "namespace" and member.get("prot") != "public":
                continue
            checked += 1
            location = member.find("location")
            location_file = "" if location is None else (location.get("file") or "")
            for header in ng_headers:
                if location_file.endswith(header):
                    observed_ng_headers[header] += 1

    if checked == 0:
        failures.append("no public callable was found in Doxygen XML")
    missing_ng_headers = sorted(
        header for header in ng_headers if observed_ng_headers[header] == 0
    )
    if missing_ng_headers:
        failures.append(
            "implemented NG catalog headers have no Doxygen public declaration: "
            + ", ".join(missing_ng_headers)
        )
    if failures:
        print("Doxygen contract validation failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1

    details = [
        f"{checked} public callables",
        f"{sum(observed_ng_headers.values())} declarations in {len(ng_headers)} catalog-admitted headers",
    ]
    print("validated Doxygen XML (" + "; ".join(details) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
