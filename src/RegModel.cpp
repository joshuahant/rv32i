#include <iostream>
#include "RegModel.h"

// This Class should model the registers in the simulation
// and control access to them.
//
RegModel::RegModel(void) {
    pc = 0;//INITIAL_VALUE;
    reg[0] = 0;
}

RegModel::~RegModel(void) {
}

bool RegModel::readReg(int index, uint32_t &val) {
    val = reg[index];

    return true;
}

bool RegModel::writeReg(int index, uint32_t val) {
    bool ret = true;

    if(index == 0) {
        ret = false;
        std::cout << "ERROR: Attempting to write to Reg 0" << std::endl;
    } else {
        reg[index] = val;
    }

    return ret;
}

uint32_t RegModel::getPC() {
    return pc;
}

uint32_t RegModel::getAndIncrementPC() {
    uint32_t previous_pc = pc;
    pc+=4;

    return previous_pc;
}
