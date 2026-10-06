#!/usr/bin/env python3
"""Compare native end-to-end shecc and GCC -O1 compilation times."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sources', nargs='+', type=Path)
    parser.add_argument('--shecc', required=True)
    parser.add_argument('--gcc', default='gcc')
    parser.add_argument('--rounds', type=int, default=7)
    parser.add_argument('--batch', type=int, default=1)
    parser.add_argument('--cpu', type=int)
    parser.add_argument('--no-libc', action='store_true')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.rounds < 3 or args.batch < 1:
        parser.error('use at least three rounds and a positive batch size')
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    compilers = {name: shutil.which(command) for name, command in
                 [('shecc', args.shecc), ('gcc', args.gcc)]}
    if not all(compilers.values()):
        parser.error('both compiler executables must exist')
    machine = platform.machine()
    elf_machine = {'x86_64': 62, 'aarch64': 183}.get(machine)
    if elf_machine is None:
        parser.error('this benchmark supports native x86-64 and ARM64 only')
    report = {'machine': machine, 'platform': platform.platform(),
              'gcc_version': subprocess.check_output(
                  [compilers['gcc'], '--version'], text=True).splitlines()[0],
              'shecc_sha256': hashlib.sha256(
                  Path(compilers['shecc']).read_bytes()).hexdigest(),
              'rounds': args.rounds, 'batch': args.batch, 'cpu': args.cpu,
              'no_libc': args.no_libc, 'workloads': []}
    with tempfile.TemporaryDirectory(prefix='shecc-compile-bench-') as temp:
        for source in args.sources:
            commands = {
                'shecc': [compilers['shecc'],
                          *(['--no-libc'] if args.no_libc else []),
                          '-o', str(Path(temp) / 'shecc.elf'), str(source)],
                'gcc': [compilers['gcc'], '-O1', '-fwrapv', '-std=c99',
                        '-o', str(Path(temp) / 'gcc.elf'), str(source)]}
            for command in commands.values():
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            for name in commands:
                with (Path(temp) / f'{name}.elf').open('rb') as binary:
                    header = binary.read(20)
                if (header[:6] != b'\x7fELF\x02\x01' or
                        int.from_bytes(header[18:20], 'little') != elf_machine):
                    parser.error(f'{name} output does not target native {machine}')
            samples = {'shecc': [], 'gcc': []}
            for round_index in range(args.rounds):
                order = ('shecc', 'gcc') if round_index % 2 == 0 else ('gcc', 'shecc')
                for name in order:
                    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
                    start = time.perf_counter()
                    for _ in range(args.batch):
                        subprocess.run(commands[name], check=True,
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
                    wall = (time.perf_counter() - start) / args.batch
                    end = resource.getrusage(resource.RUSAGE_CHILDREN)
                    cpu = (end.ru_utime + end.ru_stime -
                           usage.ru_utime - usage.ru_stime) / args.batch
                    samples[name].append({'wall_seconds': wall, 'cpu_seconds': cpu})
            medians = {name: {metric: statistics.median(s[metric] for s in values)
                             for metric in ('wall_seconds', 'cpu_seconds')}
                       for name, values in samples.items()}
            result = {'source': str(source),
                      'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'commands': commands, 'samples': samples, 'medians': medians,
                      'wall_speedup': medians['gcc']['wall_seconds'] /
                                      medians['shecc']['wall_seconds'],
                      'cpu_speedup': medians['gcc']['cpu_seconds'] /
                                     medians['shecc']['cpu_seconds']}
            report['workloads'].append(result)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            print(f'{source.name}: shecc {medians["shecc"]["wall_seconds"]:.4f}s, '
                  f'GCC {medians["gcc"]["wall_seconds"]:.4f}s, '
                  f'speedup {result["wall_speedup"]:.2f}x', flush=True)


if __name__ == '__main__':
    main()
