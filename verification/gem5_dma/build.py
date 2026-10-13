"""Isolated build of the retained protocol plus the VER-05 directory DMA fix.

Copies the retained-protocol source snapshot and object tree (frozen inputs are
only read), applies verification/rumur/v9_dma/dir_dma_fix.proposed.patch, then
builds with the retained recipe (scons -j8 to objects, manifest link with gold).
"""
import os, json, shutil, subprocess, time, hashlib, sys, signal
from pathlib import Path
R = Path('/data2/rsappidi/cacheflex_mc'); OLD = R / 'results/retained_protocol_restart_20261002'
B = R / 'results/gem5_dma_fix_20261013'; W = B / 'gem5'; G = 2**30
PATCH = R / 'verification/rumur/v9_dma/dir_dma_fix.proposed.patch'
DIR_SM = 'src/mem/ruby/protocol/MESI_Three_Level_SPM-dir.sm'
sys.path[:0] = [str(R / 'results/parallel_overlap_20260930'), str(R / 'scripts')]
import broker
from parallel_budget_scheduler import owned, memory


def save(p, d): p.write_text(json.dumps(d, indent=2) + '\n')
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def status(stage, **kw): save(B / 'build_status.json', dict(stage=stage, pid=os.getpid(), updated=time.time(), **kw))


def main():
    cfg = json.loads((R / 'results/parallel_budget_20260929/budget.json').read_text())
    os.sched_setaffinity(0, cfg['cpu_set'][:8]); os.nice(10)
    B.mkdir(exist_ok=False)
    with broker.locked() as b:
        assert broker.total(b) + 32 * G <= b['cap']
        b['leases']['dma_fix_build'] = dict(bytes=32 * G, owner=os.getpid(), family='dma fix build'); b['heartbeat'] = time.time()
    try:
        status('copying_isolated_tree')
        shutil.copytree(OLD / 'gem5', W, ignore=shutil.ignore_patterns('build', '.git', '__pycache__', 'm5out'), symlinks=True)
        (W / 'build').mkdir()
        subprocess.run(['cp', '-a', '--reflink=auto', str(OLD / 'gem5/build/ARM_MESI_Three_Level_SPM'), str(W / 'build')], check=True)
        # The copied source must be exactly the retained snapshot before patching.
        ref = json.loads((OLD / 'source_manifest.json').read_text())
        bad = [k for k, h in ref.items() if sha(W / k) != h]
        assert not bad, ('copied source differs from retained manifest', bad[:5])
        subprocess.run(['patch', '-p2', '--forward', '-i', str(PATCH)], cwd=W, check=True)
        files = [p for p in (W / 'src').rglob('*') if p.is_file()]
        man = {str(p.relative_to(W)): sha(p) for p in files}; save(B / 'source_manifest.json', man)
        changed = sorted(k for k in set(man) | set(ref) if man.get(k) != ref.get(k))
        assert changed == [DIR_SM], changed
        save(B / 'inputs.json', dict(retained_source_manifest=sha(OLD / 'source_manifest.json'), patch=str(PATCH), patch_sha256=sha(PATCH),
                                     changed=changed, dir_sm_before=ref[DIR_SM], dir_sm_after=man[DIR_SM], retained_binary=sha(OLD / 'build/gem5.opt')))
        env = dict(os.environ, LD_LIBRARY_PATH='/data2/rsappidi/conda-envs/cacheflex-gem5/lib',
                   PYTHON_CONFIG='/data2/rsappidi/conda-envs/cacheflex-gem5/bin/python3-config', PROTOC='/usr/bin/protoc',
                   CCFLAGS_EXTRA='-gdwarf-2', GEM5PY_CCFLAGS_EXTRA='-gdwarf-2', LINKFLAGS_EXTRA='-L/usr/lib64', CCACHE_DISABLE='1',
                   CACHEFLEX_LINK_MANIFEST=str(B / 'link_manifest.json'))
        cmd = ['/data2/rsappidi/conda-envs/cacheflex-gem5/bin/scons', 'build/ARM_MESI_Three_Level_SPM/gem5.opt', '-j8', '--linker=gold']
        with (B / 'build.log').open('w') as f:
            p = subprocess.Popen(cmd, cwd=W, env=env, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
            status('building', child=p.pid); paused = False
            while p.poll() is None:
                m = memory(); rss = sum(x['rss'] for x in owned().values())
                hold = rss > cfg['ram_limit'] - 4 * G or m['MemAvailable'] < 64 * G + max(0, cfg['ram_limit'] - rss)
                if hold != paused:
                    os.killpg(p.pid, signal.SIGSTOP if hold else signal.SIGCONT); paused = hold
                with broker.locked() as b: b['heartbeat'] = time.time()
                status('paused_pressure' if hold else 'building', child=p.pid, user_rss_bytes=rss); time.sleep(5)
            assert p.returncode == 0, ('build exit', p.returncode)
        out = B / 'build'; out.mkdir()
        link = json.loads((B / 'link_manifest.json').read_text()); argv = link['argv']; argv[argv.index('-o') + 1] = str(out / 'gem5.opt')
        argv = [x.replace('--thread-count=8', '--thread-count=2') for x in argv] + ['-Wl,--no-keep-files-mapped', '-Wl,--no-map-whole-files']
        status('linking')
        with (B / 'link.log').open('w') as f:
            subprocess.run(['prlimit', f'--as={24 * G}:', '--', *argv], cwd=W / 'build/ARM_MESI_Three_Level_SPM', env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
        shutil.copytree(OLD / 'build/configs', out / 'configs', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        save(out / 'frozen.json', {str(p): sha(p) for p in out.rglob('*') if p.is_file()})
        status('built_requires_runtime_validation', sha256=sha(out / 'gem5.opt'))
    except BaseException as e:
        status('failed', error=repr(e)); raise
    finally:
        with broker.locked() as b: b['leases'].pop('dma_fix_build', None); b['heartbeat'] = time.time()


if __name__ == '__main__':
    main()
