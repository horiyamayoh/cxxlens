#!/usr/bin/env python3
"""Verify callable documentation contracts through Doxygen XML."""

from __future__ import annotations

import pathlib
import sys
import xml.etree.ElementTree as ET


def text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext()).strip()


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} XML_DIRECTORY", file=sys.stderr)
        return 2
    xml_directory = pathlib.Path(sys.argv[1])
    failures: list[str] = []
    checked = 0

    for xml_file in sorted(xml_directory.glob("*.xml")):
        root = ET.parse(xml_file).getroot()
        for member in root.findall(".//memberdef[@kind='function'][@prot='public']"):
            checked += 1
            qualified_name = text(member.find("qualifiedname")) or text(member.find("name"))
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
    if failures:
        print("Doxygen contract validation failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"validated Doxygen contracts for {checked} public callables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
