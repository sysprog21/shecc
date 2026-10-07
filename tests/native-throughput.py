#!/usr/bin/env python3
"""Manual native ARM64/x86-64 checksum and throughput comparison; no privileged changes."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import resource
import shutil
import statistics
import subprocess
import time

NAMES = ('rng', 'branch', 'pointer', 'recursion', 'calls', 'sort', 'indexed')
FAILURE_OUTPUT = None
EXPECTED = (375426474, 2594414144, 2764537856, 7856720, 447236269,
            3540951488, 3908711356)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout):
    try:
        return subprocess.run(command, check=True, capture_output=True, timeout=timeout)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        if FAILURE_OUTPUT is not None:
            failure = {'passed': False, 'command': command, 'error': str(error),
                       'stdout': (error.stdout or b'').decode(errors='replace'),
                       'stderr': (error.stderr or b'').decode(errors='replace')}
            (FAILURE_OUTPUT / 'failure.json').write_text(json.dumps(failure, indent=2) + '\n')
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shecc', type=pathlib.Path, default=pathlib.Path('out/shecc'))
    parser.add_argument('--gcc', default='gcc')
    parser.add_argument('--output', type=pathlib.Path, required=True,
                        help='new directory for binaries, commands, and results')
    parser.add_argument('--cpu', type=int, help='default: last CPU in allowed affinity')
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--runs', type=int, default=8)
    parser.add_argument('--target', type=float, default=0.8,
                        help='minimum GCC/shecc throughput in both wall and CPU time for every fixture')
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    machine = platform.machine().lower()
    if machine not in ('aarch64', 'arm64', 'x86_64', 'amd64'):
        parser.error('native ARM64 or x86-64 host required; emulated timing is unsupported')
    if args.rounds < 1 or args.runs < 1 or not 0 < args.target <= 1:
        parser.error('rounds/runs must be positive and target must be in (0, 1]')
    native_machine = 183 if machine in ('aarch64', 'arm64') else 62
    allowed = os.sched_getaffinity(0)
    cpu = max(allowed) if args.cpu is None else args.cpu
    if cpu not in allowed:
        parser.error('selected CPU is outside allowed affinity')
    os.sched_setaffinity(0, {cpu})
    fixtures = pathlib.Path(__file__).resolve().with_suffix('')
    shecc = args.shecc.resolve(strict=True)
    gcc = pathlib.Path(shutil.which(args.gcc) or args.gcc).resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    global FAILURE_OUTPUT
    FAILURE_OUTPUT = output
    sources = [fixtures / (name + '.c') for name in NAMES] + [fixtures / 'start.S']
    tracked = sources + [shecc, gcc, pathlib.Path(__file__).resolve()]
    before = {str(path): sha(path) for path in tracked}
    governor = pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor')
    proof = {'scope': f'seven synthetic native {machine} workloads',
             'settings': vars(args) | {'output': str(output), 'shecc': str(shecc)},
             'environment': {'platform': platform.platform(), 'cpu': cpu,
                             'allowed_cpus': sorted(allowed), 'load_before': os.getloadavg(),
                             'governor': governor.read_text().strip() if governor.exists() else None,
                             'gcc_version': run([str(gcc), '--version'], args.timeout).stdout.decode()},
             'sha256_before': before, 'commands': {}, 'results': {}}
    for name in NAMES:
        source = fixtures / (name + '.c')
        commands = {
            'shecc': [str(shecc), '--no-libc', '-o',
                      str(output / (name + '.shecc')), str(source)],
            'gcc': [str(gcc), '-O1', '-fwrapv', '-nostdlib', '-static', '-fno-pie',
                    '-no-pie', '-Wl,--build-id=none', str(fixtures / 'start.S'),
                    str(source), '-o', str(output / (name + '.gcc'))]}
        proof['commands'][name] = commands
        for command in commands.values():
            run(command, args.timeout)
        for variant in commands:
            binary = output / (name + '.' + variant)
            with binary.open('rb') as stream:
                header = stream.read(20)
            if (header[:6] != b'\x7fELF\x02\x01' or
                    int.from_bytes(header[18:20], 'little') != native_machine):
                parser.error('compiled binary does not match the native host')
            binary.chmod(binary.stat().st_mode | 0o111)
            tracked.append(binary)
            before[str(binary)] = sha(binary)
    for name, expected in zip(NAMES, EXPECTED):
        binaries = {variant: output / (name + '.' + variant) for variant in ('shecc', 'gcc')}
        for binary in binaries.values():
            run([str(binary)], args.timeout)  # full 32-bit checksum is checked by main
        wall = {variant: [] for variant in binaries}
        cpu_samples = {variant: [] for variant in binaries}
        for round_index in range(args.rounds):
            order = tuple(binaries) if round_index % 2 == 0 else tuple(reversed(binaries))
            for variant in order:
                start_cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
                start_wall = time.perf_counter()
                for _ in range(args.runs):
                    run([str(binaries[variant])], args.timeout)
                elapsed = time.perf_counter() - start_wall
                end_cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
                wall[variant].append(elapsed / args.runs)
                cpu_samples[variant].append((end_cpu.ru_utime + end_cpu.ru_stime -
                                            start_cpu.ru_utime - start_cpu.ru_stime) / args.runs)
        medians = {kind: {variant: statistics.median(values) for variant, values in samples.items()}
                   for kind, samples in [('wall', wall), ('cpu', cpu_samples)]}
        throughput = {kind: values['gcc'] / values['shecc'] for kind, values in medians.items()}
        proof['results'][name] = {'expected_checksum': expected, 'checksum_executions':
                                 2 * (1 + args.rounds * args.runs), 'medians': medians,
                                 'wall_samples': wall, 'cpu_samples': cpu_samples,
                                 'throughput': throughput, 'target_pass': min(throughput.values()) >= args.target}
        print(name, json.dumps(throughput), flush=True)
        (output / 'results.json').write_text(json.dumps(proof, indent=2) + '\n')
    after = {str(path): sha(path) for path in tracked}
    proof['sha256_after'] = after
    proof['environment']['load_after'] = os.getloadavg()
    proof['unchanged'] = before == after
    proof['passed'] = proof['unchanged'] and all(row['target_pass'] for row in proof['results'].values())
    (output / 'results.json').write_text(json.dumps(proof, indent=2) + '\n')
    return 0 if proof['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
