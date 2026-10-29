from m5.params import *
from m5.SimObject import SimObject

class MicroResourcePool(SimObject):
    type = 'MicroResourcePool'
    cxx_header = 'mem/ruby/structures/MicroResourcePool.hh'
    cxx_class = 'gem5::ruby::MicroResourcePool'
    capacity = Param.Unsigned(0, 'Transaction credits; zero disables this model')
