#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Helper scheduling in a run's logs (src/pw_helper_sched.h): what each helper loader said of its threads
(native/proton_wine_child.c), the memory helper's scheduling census, and the two verdicts drawn from them:

  scheduled(record)  every helper that ran a program got the scheduling it asked for (the permanent check);
  c2_row(record)     from a census run (WINE_PS5_SCHED_CENSUS=1), which priority and CPUs the program's threads should
                     take so that no payload daemon, SceShell* process or the title is starved by them (C2 of
                     klog/salvage-2026-10-14/review/plan.md).

Pure: lines in, a record out. tools/run-console.py puts summary(lines) in a run's evidence."""
import re

FIFO, RR = 1, 3
HIGHEST, LOWEST = 256, 767          # the console's scale: 256 is the highest priority
TITLE_CPUS = 0x1fff                 # CPUs 0-12: what WINE_CPU_TOPOLOGY tells a Windows program (native/proton_wine.cpp)
FIELD = re.compile(r'(\w+)=(\S+)')


def _pair(text):
    first, _, second = text.partition('/')
    return int(first), int(second)


def digest(lines):
    """The scheduling lines of a run, in order: the helpers' entry lines, each Wine worker's line with the entry line
    before it (one helper's log is read whole), the program-thread tallies, the last census header, and each
    process's last census line with its thread groups."""
    record = {'helpers': [], 'workers': [], 'tallies': [], 'census': None, 'processes': {}, 'groups': {}}
    for line in lines:
        at = line.find('wine-child: sched ')
        if at >= 0:
            fields = dict(FIELD.findall(line[at:]))
            if 'kind' in fields:
                record['helpers'].append(fields)
            elif 'worker' in fields:
                record['workers'].append(dict(fields, helper=record['helpers'][-1] if record['helpers'] else None))
            elif 'set' in fields:
                record['tallies'].append(fields)
            continue
        at = line.find('sched-census: ')
        if at < 0:
            continue
        fields = dict(FIELD.findall(line[at:]))
        if 'self_tid' in fields:
            record['census'] = fields
        elif 'comm' in fields:
            record['processes'][int(fields['pid'])] = fields
            record['groups'][int(fields['pid'])] = []
        elif 'rtprio' in fields and int(fields['pid']) in record['groups']:
            record['groups'][int(fields['pid'])].append(fields)
    return record


def scheduled(record):
    """(ok, reasons): every program helper took its settings and plan, each Wine worker in rr mode took RR (the kernel's
    class changed from a plain FIFO thread's, so it is not only libthr's copy that says so), and no Windows thread kept
    its scheduling. A run with no helper, or one that kept today's scheduling, has nothing to check."""
    reasons = []
    for fields in record['helpers']:
        if fields.get('kind') == 'program' and (fields.get('error') != '0' or fields.get('service_status') != '0'):
            reasons.append(f"helper settings or service plan refused: error={fields.get('error')} "
                           f"service_status={fields.get('service_status')}")
    for worker in record['workers']:
        helper = worker['helper'] or {}
        if helper.get('mode') != 'rr':
            continue
        policy = _pair(helper['plain'])[0]
        kernel, plain_kernel = _pair(worker['worker_kernel']), _pair(helper['plain_kernel'])
        if worker.get('status') != '0' or _pair(worker['worker'])[0] != RR:
            reasons.append(f"Wine's first thread kept its scheduling: status={worker.get('status')} "
                           f"worker={worker['worker']}")
        elif kernel[0] <= 0:
            reasons.append(f'the kernel view of a thread was refused: errno {-kernel[0]}')
        elif policy == FIFO and kernel[0] == plain_kernel[0]:
            reasons.append(f"the kernel kept FIFO's class: worker_kernel={worker['worker_kernel']} "
                           f"plain_kernel={helper['plain_kernel']}")
    failed = max((int(t['failed']) for t in record['tallies']), default=0)
    if failed:
        reasons.append(f'{failed} Windows threads kept their scheduling')
    return not reasons, reasons


def c2_row(record):
    """The C2 decision, from a census run. D is a program helper's plain priority (what its threads get unless told),
    H its CPUs; the memory helper's stand in when no program helper ran (the same spawn path). Every process the census
    chose but the helpers (SceSpZeroConf) is a daemon here, the title among them, with its first thread's priority on
    the program's scale (sched_getparam) and every CPU it may or did run on. Only daemons sharing H count:
      all better than D           daemons-better: the rr default is enough;
      one equal to D, none worse  daemons-equal:  WINE_PS5_HELPER_PRIORITY=D,D+1;
      one worse than D            daemons-worse:  WINE_PS5_HELPER_PRIORITY=D,worst+1;
      worst+1 past 767            stop: no priority is worse, so no game run.
    When H holds CPUs 13-15 that a daemon runs on, and some of 0-12 too, H keeps to 0-12 (WINE_PS5_HELPER_CPUS) before
    the priorities are compared. When H holds none of 0-12 there is no CPU lever (cpu_lever False): the priority is the
    only one. Daemons whose priority could not be read are listed as unknown, those whose threads the kernel puts in more
    than one class or priority as mixed: their other threads may sit elsewhere than the first one.
      any shared daemon unknown or mixed  undecided: no verdict, so no game run."""
    if not record['processes']:
        return {'row': 'no-census'}
    helper = next((f for f in record['helpers'] if f.get('kind') == 'program'), None) or \
        next((f for f in record['helpers'] if f.get('kind') == 'memory'), None)
    if not helper:
        return {'row': 'no-helper'}
    policy, d = _pair(helper['plain'])
    if policy not in (FIFO, RR) or not HIGHEST <= d <= LOWEST:
        return {'row': 'unmeasured', 'plain': helper['plain']}
    h = int(helper['entry_cpus'], 0)
    daemons = []
    for pid, fields in sorted(record['processes'].items()):
        if fields['comm'] == 'SceSpZeroConf':
            continue
        groups = record['groups'].get(pid, [])
        cpus = int(fields['cpus'], 0)
        for group in groups:
            cpus |= int(group['cpus'], 0)
        last = int(fields['lastcpu'])
        if 0 <= last < 64:
            cpus |= 1 << last
        priority = int(fields['first_prio']) if int(fields['first_policy']) in (FIFO, RR) else None
        daemons.append({'pid': pid, 'comm': fields['comm'], 'priority': priority, 'cpus': hex(cpus),
                        'kernel_views': len({group['rtprio'] for group in groups})})
    env = []
    if h & ~TITLE_CPUS and h & TITLE_CPUS and any(int(dm['cpus'], 16) & h & ~TITLE_CPUS for dm in daemons):
        h &= TITLE_CPUS
        env.append(f'WINE_PS5_HELPER_CPUS={h:#x}')
    shared = [dm for dm in daemons if not h or not int(dm['cpus'], 16) or int(dm['cpus'], 16) & h]
    worst = max((dm['priority'] for dm in shared if dm['priority'] is not None), default=None)
    row = {'D': d, 'H': f'{h:#x}', 'cpu_lever': not h or bool(h & TITLE_CPUS),
           'unknown': [dm['comm'] for dm in shared if dm['priority'] is None],
           'mixed': [dm['comm'] for dm in shared if dm['kernel_views'] > 1], 'daemons': daemons}
    if row['unknown'] or row['mixed']:
        return dict(row, row='undecided', env=env)
    if worst is None or worst < d:
        return dict(row, row='daemons-better', env=env)
    program = d + 1 if worst == d else worst + 1
    if program > LOWEST:
        return dict(row, row='stop', env=[], worst=worst)
    env.append(f'WINE_PS5_HELPER_PRIORITY={d},{program}')
    return dict(row, row='daemons-equal' if worst == d else 'daemons-worse', env=env, worst=worst)


def summary(lines):
    """The evidence record of a run, or None when no helper said anything of its scheduling."""
    record = digest(lines)
    if not record['helpers'] and not record['processes']:
        return None
    ok, reasons = scheduled(record)
    tallies = record['tallies']
    return {'helpers': record['helpers'],
            'workers': [{k: v for k, v in worker.items() if k != 'helper'} for worker in record['workers']],
            'program_threads': {'set': max(int(t['set']) for t in tallies),
                                'failed': max(int(t['failed']) for t in tallies)} if tallies else None,
            'census': record['census'], 'census_processes': len(record['processes']),
            'row': c2_row(record) if record['processes'] else None,
            'scheduled': {'ok': ok, 'reasons': reasons}}
