#include <iostream>
#include "common.hpp"
#include "RegModel.h"

int main(){
    RegModel reg;
    uint32_t i = 0;
    if(reg.writeReg(2, 30)) {
        reg.readReg(2, i);
        std::cout << "Successfully wrote and read " << i << std::endl;
    } else {
        std::cout << "Failed to write to reg" << std::endl;
    }

    return 0;
}

