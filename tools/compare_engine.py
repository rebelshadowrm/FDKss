"""Compare FDK RTTI and exact leaf getters across explicitly selected PE builds.

Writes per-build evidence and a comparison matrix; it never creates runtime
profiles, attaches to games or treats shared names as ABI compatibility.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re

from dump_sdk import recover
from discover_engine_getters import discover
from discover_engine_threads import discover as discover_threads


def parse_build(value):
    label, separator, path = value.partition('=')
    if not separator or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', label) or not path:
        raise ValueError('build must be LABEL=EXECUTABLE, with a simple unique label')
    return label, Path(path)


def compare(builds, output, base_class='.?AVActor@fdk@@'):
    import pefile
    if not 2 <= len(builds) <= 32:
        raise ValueError('comparison requires between 2 and 32 builds')
    labels = [label for label, _ in builds]
    if len({label.casefold() for label in labels}) != len(labels):
        raise ValueError('build labels must be unique, including case')
    if any(not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', label) for label in labels):
        raise ValueError('invalid build label')
    outputs = [output / 'comparison.json'] + [output / (label + suffix) for label in labels
                                              for suffix in ('-rtti.json', '-getters.json', '-threads.json')]
    for target in outputs:
        for _, source in builds:
            if target.resolve() == source.resolve() or target.exists() and target.samefile(source):
                raise ValueError('comparison output would overwrite an executable')
    summary = dict(format='fdkss.engine_comparison', version=2,
                   observed_utc=datetime.now(timezone.utc).isoformat(), base_class=base_class,
                   builds={}, shared_rtti_names=[], getter_matrix=[], thread_matrix=[], runtime_support_inferred=False,
                   limitations=['Shared RTTI establishes type-name overlap, not layout/ABI/lifetime equivalence.',
                                'Getter descriptions are exact instruction effects, not approved field schemas or calls.',
                                'Thread predicates describe ID comparisons; Actor ownership and scheduling are not inferred.',
                                'No game process, runtime adapter, update owner or lifetime is examined.'])
    names, descriptions, threads, reports = {}, {}, {}, {}
    for label, path in builds:
        data = path.read_bytes()
        pe = pefile.PE(data=data)
        classes = recover(pe)
        getters = discover(pe, classes, base_class)
        predicates = discover_threads(pe, classes)
        digest = hashlib.sha256(data).hexdigest(); getters['build_sha256'] = digest
        predicates['build_sha256'] = digest
        threads[label] = predicates
        names[label] = {c['name'] for c in classes}
        descriptions[label] = {c['class_name']: c for c in getters['candidates']}
        reports[output / (label + '-rtti.json')] = classes
        reports[output / (label + '-getters.json')] = getters
        reports[output / (label + '-threads.json')] = predicates
        summary['builds'][label] = dict(path=str(path.resolve()), build_sha256=digest,
            bytes=len(data), machine=hex(pe.FILE_HEADER.Machine), image_base=hex(pe.OPTIONAL_HEADER.ImageBase),
            rtti_classes=len(classes), described_classes=len(getters['candidates']),
            rejected_classes=len(getters['rejected']),
            thread_predicates=len(predicates['candidates']),
            thread_rejected_classes=len(predicates['rejected']),
            rejection_reasons=dict(Counter(r['reason'] for r in getters['rejected'])))
    summary['shared_rtti_names'] = sorted(set.intersection(*names.values()))
    for name in sorted(set.union(*(set(d) for d in descriptions.values()))):
        rows = {}
        for label in labels:
            cls = descriptions[label].get(name)
            rows[label] = dict(vtable_rva=cls['vtable_rva'], getters=cls['getters']) if cls else None
        summary['getter_matrix'].append(dict(class_name=name, builds=rows))
    thread_classes = {row['class_name'] for report in threads.values() for row in report['candidates']}
    for name in sorted(thread_classes):
        summary['thread_matrix'].append(dict(class_name=name, builds={
            label: [row for row in threads[label]['candidates'] if row['class_name'] == name]
            for label in labels}))
    reports[output / 'comparison.json'] = summary
    # Validate every source before publishing any evidence from this invocation.
    output.mkdir(parents=True, exist_ok=True)
    for target, report in reports.items():
        target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return summary


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='append', required=True, help='LABEL=EXECUTABLE; repeat for every build')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--base-class', default='.?AVActor@fdk@@')
    args = parser.parse_args()
    try:
        summary = compare([parse_build(value) for value in args.build], args.output, args.base_class)
    except (ValueError, OSError, pefile.PEFormatError) as error:
        parser.error(str(error))
    for label, row in summary['builds'].items():
        print(f"{label}: {row['rtti_classes']} RTTI classes; {row['described_classes']} getter descriptions; {row['rejected_classes']} rejected")
    print(f"{len(summary['shared_rtti_names'])} shared RTTI names; runtime support is not inferred")


if __name__ == '__main__':
    main()
