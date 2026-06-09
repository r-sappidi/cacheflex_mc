import argparse

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.ruby.mesi_three_level_cache_hierarchy import (
    MESIThreeLevelCacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.complex_generator import ComplexGenerator
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires
from m5.ticks import fromSeconds
from m5.util.convert import toLatency, toMemoryBandwidth

parser = argparse.ArgumentParser()
parser.add_argument("--kernel", choices=["gemm", "flash"], required=True)
parser.add_argument("--mode", choices=["private", "shared"], required=True)
parser.add_argument("--cores", type=int, required=True)
parser.add_argument("--duration", default="200ns")
parser.add_argument("--rate", default="16GiB/s")
parser.add_argument("--data-limit", type=int, default=16384)
args = parser.parse_args()

requires(coherence_protocol_required=CoherenceProtocol.MESI_THREE_LEVEL)

read_percent = 55 if args.kernel == "gemm" else 80
max_addr = 1 << 20


def make_strided(core_id):
    def traffic(pygen):
        block_size = 8
        superblock_size = 8
        stride_size = 64 if args.mode == "shared" else args.cores * 64
        offset = core_id * (8 if args.mode == "shared" else 64)
        period = fromSeconds(block_size / toMemoryBandwidth(args.rate))
        yield pygen.createStrided(
            fromSeconds(toLatency(args.duration)),
            0,
            max_addr,
            offset,
            block_size,
            superblock_size,
            stride_size,
            period,
            period,
            read_percent,
            args.data_limit,
        )
        yield pygen.createExit(0)

    return traffic


generator = ComplexGenerator(num_cores=args.cores)
for i, core in enumerate(generator.cores):
    core.set_traffic_from_python_generator(make_strided(i))

memory = SingleChannelDDR3_1600(size="64MiB")
cache_hierarchy = MESIThreeLevelCacheHierarchy(
    l1i_size="16KiB",
    l1i_assoc=2,
    l1d_size="16KiB",
    l1d_assoc=2,
    l2_size="64KiB",
    l2_assoc=4,
    l3_size="1MiB",
    l3_assoc=8,
    num_l3_banks=max(1, args.cores // 2),
)
board = TestBoard(
    clk_freq="2GHz",
    generator=generator,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

simulator = Simulator(board=board)
simulator.run()
