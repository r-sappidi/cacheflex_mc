# Copyright (c) 2020 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2009 Advanced Micro Devices, Inc.
# Copyright (c) 2020 ARM Limited
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.ClockedObject import ClockedObject
from m5.objects.RubyCache import RubyCache
from m5.objects.RubySystem import RubySystem
from m5.objects.System import System
from m5.params import *
from m5.proxy import *


class RubyPort(ClockedObject):
    type = "RubyPort"
    abstract = True
    cxx_header = "mem/ruby/system/RubyPort.hh"
    cxx_class = "gem5::ruby::RubyPort"

    version = Param.Int(0, "")

    in_ports = VectorResponsePort(
        "CPU side of this RubyPort/Sequencer. "
        "The CPU request ports should be connected to this. If a CPU "
        "has multiple ports (e.g., I/D ports) all of the ports for a "
        "single CPU can connect to one RubyPort."
    )

    interrupt_out_port = VectorRequestPort(
        "Port to connect to x86 interrupt "
        "controller to send the CPU requests from outside."
    )

    pio_request_port = RequestPort("Ruby pio request port")

    mem_request_port = RequestPort("Ruby mem request port")

    pio_response_port = ResponsePort("Ruby pio response port")

    using_ruby_tester = Param.Bool(False, "")
    no_retry_on_stall = Param.Bool(False, "")
    ruby_system = Param.RubySystem("Parent RubySystem object")
    system = Param.System(Parent.any, "system object")
    support_data_reqs = Param.Bool(True, "data cache requests supported")
    support_inst_reqs = Param.Bool(True, "inst cache requests supported")
    cacheflex_ifetch_same_tick_response = Param.Bool(False,
        'DIAGNOSTIC: deliver completed IFETCH immediately when response queue '
        'is empty; retain cache timing and normal queued retry fallback')
    is_cpu_sequencer = Param.Bool(True, "connected to a cpu")


class RubyPortProxy(RubyPort):
    type = "RubyPortProxy"
    cxx_header = "mem/ruby/system/RubyPortProxy.hh"
    cxx_class = "gem5::ruby::RubyPortProxy"


class RubySequencer(RubyPort):
    type = "RubySequencer"
    cacheflex_reduction = Param.Bool(False, 'Experimental bounded FP32 SPM scale/add engine')
    cacheflex_l3_reduction = Param.Bool(False, 'Route captured scale/add commands to shared LLC bank engines')
    cacheflex_reduction_cache = Param.RubyCache(NULL, 'Existing private L2 SRAM; used only to validate claimed destinations')
    cacheflex_reduction_async = Param.Bool(True, 'ACK after payload capture, not SPM installation')
    cacheflex_reduction_source_entries = Param.Unsigned(4, 'Core-side 256B contribution records')
    cacheflex_reduction_sink_entries = Param.Unsigned(4, 'Near-SPM contribution records, including active update')
    cacheflex_reduction_link_bytes = Param.Unsigned(16, 'Dedicated local command link bytes/cycle, including 16B header')
    cacheflex_reduction_link_latency = Param.Cycles(2, 'Local command link fixed latency')
    cacheflex_reduction_lanes = Param.Unsigned(16, 'FP32 lanes/cycle in serialized near-SPM engine')
    cacheflex_reduction_mul_latency = Param.Cycles(3, 'FP32 multiply pipeline latency')
    cacheflex_reduction_add_latency = Param.Cycles(3, 'FP32 add pipeline latency; non-fused arithmetic')
    cacheflex_early_read_pcs = VectorParam.Addr([], 'DIAGNOSTIC: private accumulator load PCs returned early; real request/credits retained')
    cacheflex_ideal_staging_cache = Param.RubyCache(NULL,
        'DIAGNOSTIC ONLY: oracle copies into existing private SPM slots; no added SRAM')
    cacheflex_ideal_staging_controller = Param.RubyController(NULL,
        'Private L2 controller used to check stable SPM slot state for oracle copies')
    cacheflex_spm_ingress = Param.MessageBuffer(NULL, 'Independent pure SPM input')
    cacheflex_micro_data_misses = Param.MicroResourcePool(NULL, 'L1D miss pool')
    cacheflex_micro_inst_misses = Param.MicroResourcePool(NULL, 'L1I miss pool')
    cacheflex_micro_writebacks = Param.MicroResourcePool(NULL, 'L1 eviction pool')
    cacheflex_micro_targets = Param.Unsigned(0, 'Target bound only while a miss is live')
    cacheflex_finite_ingress = Param.Bool(False,
        "Respect finite controller mandatory queue space, including alias replay")
    cacheflex_max_line_targets = Param.Unsigned(0,
        "Finite per-line coalescing target records (0 retains legacy behavior)")
    cacheflex_max_bridge_requests = Param.Int(0,
        "Cap bridge requests within shared total; ordinary requests may use all free credits (0 disables)")
    cxx_class = "gem5::ruby::Sequencer"
    cxx_header = "mem/ruby/system/Sequencer.hh"

    dcache = Param.RubyCache("")

    max_outstanding_requests = Param.Int(
        16, "max requests (incl. prefetches) outstanding"
    )
    cacheflex_fetch_credits = Param.Int(0, "Opt-in dedicated IFETCH credits within total cap; zero shares")
    cacheflex_bridge_credits = Param.Int(0, "Opt-in dedicated SPMCP/SPMWB credits within total cap; zero shares")
    deadlock_threshold = Param.Cycles(
        500000,
        "max outstanding cycles for a request "
        "before deadlock/livelock declared",
    )
    garnet_standalone = Param.Bool(False, "")
    # id used by protocols that support multiple sequencers per controller
    # 99 is the dummy default value
    coreid = Param.Int(99, "CorePair core id")

    def connectCpuPorts(self, cpu):
        """
        Helper for connecting all cpu memory request output ports to this
        object's in_ports.
        This assumes the provided cpu object is an instance of BaseCPU. Non-cpu
        objects should use connectInstPort and connectDataPort.
        """
        import m5.objects

        assert isinstance(cpu, m5.objects.BaseCPU)
        # this connects all cpu mem-side ports to self.in_ports
        cpu.connectAllPorts(
            self.in_ports, self.in_ports, self.interrupt_out_port
        )

    def connectIOPorts(self, piobus):
        """
        Helper for connecting this object's IO request and response ports to
        the provided bus object. Usually a iobus object is used to wireup IO
        components in a full system simulation. Incoming/Outgoing IO requests
        do not go though the SLICC protocol so the iobus must be connected to
        the sequencer directly.
        """
        self.pio_request_port = piobus.cpu_side_ports
        self.mem_request_port = piobus.cpu_side_ports
        self.pio_response_port = piobus.mem_side_ports


class RubyHTMSequencer(RubySequencer):
    type = "RubyHTMSequencer"
    cxx_class = "gem5::ruby::HTMSequencer"
    cxx_header = "mem/ruby/system/HTMSequencer.hh"


class DMASequencer(RubyPort):
    type = "DMASequencer"
    cxx_header = "mem/ruby/system/DMASequencer.hh"
    cxx_class = "gem5::ruby::DMASequencer"

    max_outstanding_requests = Param.Int(64, "max outstanding requests")
