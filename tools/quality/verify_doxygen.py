#!/usr/bin/env python3
"""Verify callable documentation contracts through Doxygen XML."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

import yaml


def text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext()).strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xml_directory", type=pathlib.Path)
    parser.add_argument("--candidate-manifest", type=pathlib.Path)
    parser.add_argument("--ng-catalog", type=pathlib.Path)
    args = parser.parse_args()
    xml_directory = args.xml_directory
    failures: list[str] = []
    checked = 0
    planned_contracts: dict[str, set[str]] = {}
    planned_required: collections.Counter[str] = collections.Counter()
    planned_headers: set[str] = set()
    if args.candidate_manifest:
        manifest = yaml.safe_load(args.candidate_manifest.read_text(encoding="utf-8"))
        root = args.candidate_manifest.resolve().parent.parent
        catalog = yaml.safe_load((root / manifest["catalog"]).read_text(encoding="utf-8"))
        implementation_states = {
            api["id"]: api["implementation_state"]
            for package in catalog["packages"]
            for api in package["apis"]
        }
        for group in manifest["groups"]:
            for contract in group["api_contracts"]:
                if implementation_states[contract["api_id"]] != "unimplemented":
                    continue
                obligation = contract["traceability"]["doxygen_obligation"]
                if not obligation:
                    failures.append(f"{contract['api_id']}: empty manifest Doxygen obligation")
                planned_headers.add(contract["declaration"]["source"])
                for signature in contract["declaration"]["signature"].split(";"):
                    matches = re.findall(
                        r"(cxxlens(?:::[A-Za-z_][A-Za-z0-9_]*)+)\s*\(", signature
                    )
                    if not matches:
                        failures.append(
                            f"{contract['api_id']}: cannot extract a Doxygen member from signature"
                        )
                        continue
                    member = matches[0]
                    planned_contracts.setdefault(member, set()).add(contract["api_id"])
                    planned_required[member] += 1
    observed_planned: collections.Counter[str] = collections.Counter()
    ng_headers: set[str] = set()
    observed_ng_headers: collections.Counter[str] = collections.Counter()
    if args.ng_catalog:
        ng_catalog = yaml.safe_load(args.ng_catalog.read_text(encoding="utf-8"))
        for entry in ng_catalog["entries"]:
            if entry["status"] != "implemented":
                continue
            if not all(
                entry[field]
                for field in ("symbols", "lifetime", "threading", "versioning", "invariants")
            ):
                failures.append(f"{entry['id']}: incomplete NG Doxygen/catalog obligation")
            ng_headers.update(entry["headers"])

    for xml_file in sorted(xml_directory.glob("*.xml")):
        root = ET.parse(xml_file).getroot()
        compound = root.find("compounddef")
        if compound is None or compound.get("kind") not in {"class", "struct", "namespace"}:
            continue
        for member in compound.findall("./sectiondef/memberdef[@kind='function']"):
            if compound.get("kind") != "namespace" and member.get("prot") != "public":
                continue
            checked += 1
            qualified_name = text(member.find("qualifiedname")) or text(member.find("name"))
            location = member.find("location")
            location_file = "" if location is None else (location.get("file") or "")
            matching_ng_headers = {
                header for header in ng_headers if location_file.endswith(header)
            }
            if matching_ng_headers:
                for header in matching_ng_headers:
                    observed_ng_headers[header] += 1
                # The sibling public_callable_inventory.py check-doxygen command
                # enforces the exact one-callable-per-row bijection. This checker
                # retains the independent documentation-quality obligation.
                continue
            if qualified_name in planned_contracts:
                observed_planned[qualified_name] += 1
                continue
            if any(location_file.endswith(header) for header in planned_headers):
                # Supporting public value accessors share the package/API obligation even when
                # they are not separate top-level catalog entries.
                continue
            brief = text(member.find("briefdescription"))
            details = member.find("detaileddescription")
            if not brief:
                failures.append(f"{qualified_name}: missing @brief")
            if details is None:
                failures.append(f"{qualified_name}: missing detailed contract")
                continue

            simple_kinds = {section.get("kind") for section in details.findall(".//simplesect")}
            for required in ("pre", "post", "note"):
                if required not in simple_kinds:
                    failures.append(f"{qualified_name}: missing @{required}")
            if details.find(".//programlisting") is None:
                failures.append(f"{qualified_name}: missing @code example")

            documented_parameters: dict[str, str | None] = {}
            for parameter_list in details.findall(".//parameterlist[@kind='param']"):
                for item in parameter_list.findall("parameteritem"):
                    for name in item.findall("./parameternamelist/parametername"):
                        documented_parameters[text(name)] = name.get("direction")
            declared_parameters = [text(param.find("declname")) for param in member.findall("param")]
            for parameter_name in filter(None, declared_parameters):
                if parameter_name not in documented_parameters:
                    failures.append(f"{qualified_name}: parameter {parameter_name} is undocumented")
                elif documented_parameters[parameter_name] not in {"in", "out", "in,out"}:
                    failures.append(f"{qualified_name}: parameter {parameter_name} lacks direction")

            return_type = "".join(text(member.find("type")).split())
            has_retval = details.find(".//parameterlist[@kind='retval']") is not None
            if return_type and return_type != "void" and not has_retval:
                failures.append(f"{qualified_name}: non-void callable is missing @retval")

    if checked == 0:
        failures.append("no public callable was found in Doxygen XML")
    missing_planned = {
        name: required - observed_planned[name]
        for name, required in planned_required.items()
        if observed_planned[name] < required
    }
    if missing_planned:
        failures.append(
            "planned catalog members absent from Doxygen XML: "
            + ", ".join(f"{name} ({count})" for name, count in sorted(missing_planned.items()))
        )
    missing_ng_headers = sorted(
        header for header in ng_headers if observed_ng_headers[header] == 0
    )
    if missing_ng_headers:
        failures.append(
            "implemented NG catalog headers have no Doxygen callable: "
            + ", ".join(missing_ng_headers)
        )
    if failures:
        print("Doxygen contract validation failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"validated Doxygen contracts for {checked} public callables "
        f"({sum(observed_planned.values())} planned declarations and "
        f"{sum(observed_ng_headers.values())} NG callables in catalog-admitted headers)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
