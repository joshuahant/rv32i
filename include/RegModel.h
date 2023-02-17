class RegModel {
    public:
        bool readReg(int index, uint32_t &val);
        bool writeReg(int index, uint32_t val);
        uint32_t getPC();
        uint32_t getAndIncrementPC();
        RegModel();
        ~RegModel();

    private:
        uint32_t reg[32];
        uint32_t pc;
};

