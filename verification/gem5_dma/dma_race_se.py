"""DMA engines on Ruby DMA sequencers, beside the retained SE configuration chain.

Each --dma-range lo:hi (physical, line aligned) gets a PyTrafficGen issuing
random full-line DMA reads/writes through its own DMA controller, so DMA
requests reach the directory as DMA_READ/DMA_WRITE. Without --dma-range this
adds nothing and runs l3_direct_reduction_se.py unchanged.
"""
from pathlib import Path
import runpy
import m5
from m5.util import addToPath
from m5.objects import PyTrafficGen
addToPath('../..')
from ruby import MESI_Three_Level_SPM as protocol

old_options = protocol.define_options
old_create = protocol.create_system
GENS = []
OPTIONS = []


def define_options(parser):
    old_options(parser)
    parser.add_argument('--dma-range', action='append', default=[])
    parser.add_argument('--dma-read-percent', type=int, default=50)
    parser.add_argument('--dma-min-period', type=int, default=20000, help='ticks')
    parser.add_argument('--dma-max-period', type=int, default=200000, help='ticks')
    parser.add_argument('--dma-progress-check', default='1ms')


def create_system(options, full_system, system, dma_ports, bootmem, ruby_system, cpus):
    OPTIONS.append(options)
    gens = []
    for spec in options.dma_range:
        lo, hi = (int(x, 0) for x in spec.split(':'))
        assert lo % 64 == 0 and hi % 64 == 0 and hi > lo
        gens.append(PyTrafficGen(progress_check=options.dma_progress_check))
        GENS.append((gens[-1], lo, hi))
    if gens:
        system.dma_gens = gens
    return old_create(options, full_system, system, list(dma_ports) + [g.port for g in gens], bootmem, ruby_system, cpus)


old_instantiate = m5.instantiate


def instantiate(*args, **kwargs):
    result = old_instantiate(*args, **kwargs)
    opts = OPTIONS[0]
    for gen, lo, hi in GENS:
        g = gen.createRandom(m5.MaxTick, lo, hi, 64, opts.dma_min_period, opts.dma_max_period, opts.dma_read_percent, 0)
        gen.start(iter([g]))
    return result


protocol.define_options = define_options
protocol.create_system = create_system
m5.instantiate = instantiate
runpy.run_path(str(Path(__file__).with_name('l3_direct_reduction_se.py')), run_name='__m5_main__')
