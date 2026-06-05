# Copyright (c) 2026
# All rights reserved.

from m5.defines import buildEnv
from m5.objects import *
from m5.util import fatal

from . import MESI_Three_Level as _mesi


class L0Cache(_mesi.L0Cache):
    pass


class L1Cache(_mesi.L1Cache):
    pass


class L2Cache(_mesi.L2Cache):
    pass


def define_options(parser):
    return _mesi.define_options(parser)


def create_system(
    options, full_system, system, dma_ports, bootmem, ruby_system, cpus
):
    if buildEnv["PROTOCOL"] != "MESI_Three_Level_SPM":
        fatal(
            "This script requires the MESI_Three_Level_SPM protocol to be built."
        )

    old_protocol = buildEnv["PROTOCOL"]
    old_l0 = getattr(_mesi, "MESI_Three_Level_L0Cache_Controller", None)
    old_l1 = getattr(_mesi, "MESI_Three_Level_L1Cache_Controller", None)
    old_l2 = getattr(_mesi, "MESI_Three_Level_L2Cache_Controller", None)
    old_dma = getattr(_mesi, "MESI_Three_Level_DMA_Controller", None)

    _mesi.MESI_Three_Level_L0Cache_Controller = (
        MESI_Three_Level_SPM_L0Cache_Controller
    )
    _mesi.MESI_Three_Level_L1Cache_Controller = (
        MESI_Three_Level_SPM_L1Cache_Controller
    )
    _mesi.MESI_Three_Level_L2Cache_Controller = (
        MESI_Three_Level_SPM_L2Cache_Controller
    )
    _mesi.MESI_Three_Level_DMA_Controller = (
        MESI_Three_Level_SPM_DMA_Controller
    )
    buildEnv["PROTOCOL"] = "MESI_Three_Level"

    try:
        return _mesi.create_system(
            options,
            full_system,
            system,
            dma_ports,
            bootmem,
            ruby_system,
            cpus,
        )
    finally:
        buildEnv["PROTOCOL"] = old_protocol
        if old_l0 is None:
            del _mesi.MESI_Three_Level_L0Cache_Controller
        else:
            _mesi.MESI_Three_Level_L0Cache_Controller = old_l0

        if old_l1 is None:
            del _mesi.MESI_Three_Level_L1Cache_Controller
        else:
            _mesi.MESI_Three_Level_L1Cache_Controller = old_l1

        if old_l2 is None:
            del _mesi.MESI_Three_Level_L2Cache_Controller
        else:
            _mesi.MESI_Three_Level_L2Cache_Controller = old_l2

        if old_dma is None:
            del _mesi.MESI_Three_Level_DMA_Controller
        else:
            _mesi.MESI_Three_Level_DMA_Controller = old_dma
