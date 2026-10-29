# Copyright (c) 2010, 2017, 2020, 2024 Arm Limited
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
# Copyright (c) 2006-2007 The Regents of The University of Michigan
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

from m5.defines import buildEnv
from m5.objects.FuncUnit import *
from m5.params import *
from m5.SimObject import SimObject


class IntALU(FUDesc):
    opList = [OpDesc(opClass="IntAlu")]
    count = 6


class IntMultDiv(FUDesc):
    opList = [
        OpDesc(opClass="IntMult", opLat=3),
        OpDesc(opClass="IntDiv", opLat=20, pipelined=False),
    ]

    count = 2


class FP_ALU(FUDesc):
    opList = [
        OpDesc(opClass="FloatAdd", opLat=2),
        OpDesc(opClass="FloatCmp", opLat=2),
        OpDesc(opClass="FloatCvt", opLat=2),
    ]
    count = 4


class FP_MultDiv(FUDesc):
    opList = [
        OpDesc(opClass="FloatMult", opLat=4),
        OpDesc(opClass="FloatMultAcc", opLat=5),
        OpDesc(opClass="FloatMisc", opLat=3),
        OpDesc(opClass="FloatDiv", opLat=12, pipelined=False),
        OpDesc(opClass="FloatSqrt", opLat=24, pipelined=False),
    ]
    count = 2


class SIMD_Unit(FUDesc):
    opList = [
        # Non-pipelined (blocking)
        OpDesc(opClass="SimdDiv",      opLat=12, pipelined=False),
        OpDesc(opClass="SimdSqrt",     opLat=12, pipelined=False),
        OpDesc(opClass="SimdFloatDiv", opLat=10, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt",opLat=12, pipelined=False),
        # Integer SIMD (pipelined)
        OpDesc(opClass="SimdAdd",        opLat=2),
        OpDesc(opClass="SimdAddAcc",     opLat=2),
        OpDesc(opClass="SimdAlu",        opLat=2),
        OpDesc(opClass="SimdCmp",        opLat=2),
        OpDesc(opClass="SimdCvt",        opLat=2),
        OpDesc(opClass="SimdShift",      opLat=2),
        OpDesc(opClass="SimdShiftAcc",   opLat=2),
        OpDesc(opClass="SimdMisc",       opLat=2),
        OpDesc(opClass="SimdConfig",     opLat=1),
        OpDesc(opClass="SimdMult",       opLat=4),
        OpDesc(opClass="SimdMultAcc",    opLat=4),
        OpDesc(opClass="SimdMatMultAcc", opLat=5),
        # Float SIMD (pipelined)
        OpDesc(opClass="SimdFloatAdd",        opLat=3),
        OpDesc(opClass="SimdFloatAlu",        opLat=3),
        OpDesc(opClass="SimdFloatCmp",        opLat=3),
        OpDesc(opClass="SimdFloatCvt",        opLat=3),
        OpDesc(opClass="SimdFloatMisc",       opLat=3),
        OpDesc(opClass="SimdFloatMult",       opLat=4),
        OpDesc(opClass="SimdFloatMultAcc",    opLat=4),   # FMLA: 4-cycle latency
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=5),
        # Reduction (tree reduction — non-pipelined: FU blocked for opLat cycles)
        # pipelined=False: new reduction cannot issue until previous finishes
        OpDesc(opClass="SimdReduceAdd",      opLat=4, pipelined=False),
        OpDesc(opClass="SimdReduceAlu",      opLat=4, pipelined=False),
        OpDesc(opClass="SimdReduceCmp",      opLat=3, pipelined=False),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=6, pipelined=False),  # svaddv FP16→scalar
        OpDesc(opClass="SimdFloatReduceCmp", opLat=4, pipelined=False),
        # Extensions / crypto
        OpDesc(opClass="SimdExt",       opLat=3),
        OpDesc(opClass="SimdFloatExt",  opLat=3),
        OpDesc(opClass="SimdAes",       opLat=2),
        OpDesc(opClass="SimdAesMix",    opLat=2),
        OpDesc(opClass="SimdSha1Hash",  opLat=2),
        OpDesc(opClass="SimdSha1Hash2", opLat=2),
        OpDesc(opClass="SimdSha256Hash",opLat=2),
        OpDesc(opClass="SimdSha256Hash2",opLat=2),
        OpDesc(opClass="SimdShaSigma2", opLat=2),
        OpDesc(opClass="SimdShaSigma3", opLat=2),
    ]
    count = 4


#jq86
# class SIMD_Unit(FUDesc):
#     opList = [
#         # ==========================================
#         # 1. 非流水线操作 (Blocking / Iterative)
#         # ==========================================
#         # 这些单元在计算完成前不能接收新指令
        
#         # --- 向量 (SVE/NEON) 除法与开方 ---
#         OpDesc(opClass='SimdDiv', opLat=12, pipelined=False),
#         OpDesc(opClass='SimdSqrt', opLat=12, pipelined=False),
#         OpDesc(opClass='SimdFloatDiv', opLat=10, pipelined=False),
#         OpDesc(opClass='SimdFloatSqrt', opLat=12, pipelined=False),

#         # --- 标量 (Scalar) 开方 [你要求的补充] ---
#         # 注意：如果你添加了标量开方，通常也应该在这里添加标量除法 (FloatDiv)
#         OpDesc(opClass='FloatSqrt', opLat=24, pipelined=False),
#         OpDesc(opClass='FloatDiv', opLat=20, pipelined=False), # 建议同时加上这个

#         # ==========================================
#         # 2. 全流水线操作 (Fully Pipelined)
#         # ==========================================
#         # SVE 核心谓词逻辑
#         OpDesc(opClass='SimdPredAlu', opLat=2), 

#         # 向量整数运算
#         OpDesc(opClass='SimdAdd', opLat=2),
#         OpDesc(opClass='SimdAddAcc', opLat=2),
#         OpDesc(opClass='SimdAlu', opLat=2),
#         OpDesc(opClass='SimdCmp', opLat=2),
#         OpDesc(opClass='SimdShift', opLat=2),
#         OpDesc(opClass='SimdShiftAcc', opLat=2),
#         OpDesc(opClass='SimdMisc', opLat=2),
#         OpDesc(opClass='SimdConfig', opLat=1),
#         OpDesc(opClass='SimdMult', opLat=4),
#         OpDesc(opClass='SimdMultAcc', opLat=4),
#         OpDesc(opClass='SimdMatMultAcc', opLat=5),

#         # 向量浮点运算
#         OpDesc(opClass='SimdFloatAdd', opLat=3),
#         OpDesc(opClass='SimdFloatAlu', opLat=3),
#         OpDesc(opClass='SimdFloatCmp', opLat=3),
#         OpDesc(opClass='SimdFloatCvt', opLat=3),
#         OpDesc(opClass='SimdFloatMisc', opLat=3),
#         OpDesc(opClass='SimdFloatMult', opLat=4),
#         OpDesc(opClass='SimdFloatMultAcc', opLat=4),
#         OpDesc(opClass='SimdFloatMatMultAcc', opLat=5),
        
#         # 向量规约
#         OpDesc(opClass='SimdReduceAdd', opLat=4),
#         OpDesc(opClass='SimdReduceAlu', opLat=4),
#         OpDesc(opClass='SimdReduceCmp', opLat=3),
#         OpDesc(opClass='SimdFloatReduceAdd', opLat=6),
#         OpDesc(opClass='SimdFloatReduceCmp', opLat=4),

#         # 加密扩展
#         OpDesc(opClass='SimdExt', opLat=3),
#         OpDesc(opClass='SimdFloatExt', opLat=3),
#         OpDesc(opClass='SimdAes', opLat=2),
#         OpDesc(opClass='SimdAesMix', opLat=2),
#         OpDesc(opClass='SimdSha1Hash', opLat=2),
#         OpDesc(opClass='SimdSha256Hash', opLat=2),
#         OpDesc(opClass='SimdShaSigma2', opLat=2),
#         OpDesc(opClass='SimdShaSigma3', opLat=2),
#     ]
#     count = 4




class Matrix_Unit(FUDesc):
    opList = [
        OpDesc(opClass="Matrix"),
        OpDesc(opClass="MatrixMov"),
        OpDesc(opClass="MatrixOP"),
    ]
    count = 1


class PredALU(FUDesc):
    opList = [OpDesc(opClass="SimdPredAlu")]
    count = 1


class ReadPort(FUDesc):
    opList = [
        OpDesc(opClass="MemRead"),
        OpDesc(opClass="FloatMemRead"),
        OpDesc(opClass="SimdUnitStrideLoad"),
        OpDesc(opClass="SimdUnitStrideMaskLoad"),
        OpDesc(opClass="SimdStridedLoad"),
        OpDesc(opClass="SimdIndexedLoad"),
        OpDesc(opClass="SimdUnitStrideFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdWholeRegisterLoad"),
    ]
    count = 0


class WritePort(FUDesc):
    opList = [
        OpDesc(opClass="MemWrite"),
        OpDesc(opClass="FloatMemWrite"),
        OpDesc(opClass="SimdUnitStrideStore"),
        OpDesc(opClass="SimdUnitStrideMaskStore"),
        OpDesc(opClass="SimdStridedStore"),
        OpDesc(opClass="SimdIndexedStore"),
        OpDesc(opClass="SimdWholeRegisterStore"),
    ]
    count = 0


class RdWrPort(FUDesc):
    opList = [
        OpDesc(opClass="MemRead"),
        OpDesc(opClass="MemWrite"),
        OpDesc(opClass="FloatMemRead"),
        OpDesc(opClass="FloatMemWrite"),
        OpDesc(opClass="SimdUnitStrideLoad"),
        OpDesc(opClass="SimdUnitStrideStore"),
        OpDesc(opClass="SimdUnitStrideMaskLoad"),
        OpDesc(opClass="SimdUnitStrideMaskStore"),
        OpDesc(opClass="SimdStridedLoad"),
        OpDesc(opClass="SimdStridedStore"),
        OpDesc(opClass="SimdIndexedLoad"),
        OpDesc(opClass="SimdIndexedStore"),
        OpDesc(opClass="SimdUnitStrideFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdWholeRegisterLoad"),
        OpDesc(opClass="SimdWholeRegisterStore"),
    ]
    count = 4


class IprPort(FUDesc):
    # IprAccess was renamed to System in this gem5 base. This preserves the
    # micro configuration's single non-pipelined system-register FU.
    opList = [OpDesc(opClass="System", opLat=3, pipelined=False)]
    count = 1
