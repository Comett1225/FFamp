#pragma once
#include "bits.h"
#include "vlc.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace absent {

void wmv2IdctAdd(uint8_t* dest, ptrdiff_t stride, int16_t* block);
void wmv2IdctPut(uint8_t* dest, ptrdiff_t stride, int16_t* block);

class IntraX8 {
public:
    void setSize(int mbWidth, int mbHeight);
    void decode(BitReader& in, uint8_t* const data[3], const int linesize[3], int8_t* qscaleTable, int dquant, int quantOffset, bool loopFilter);

private:
    int dcLevel(int mode, int& level, int& final);
    void acRunLevel(int mode, int& run, int& level, int& final);
    void selectAcTable(int mode);
    int orientation();
    bool spatialPredictor(int chroma);
    void prediction();
    void chromaPrediction();
    bool intraBlock(int chroma);
    void acCompensation(int direction, int dcLevel);

    BitReader* gb = nullptr;
    const Vlc* acTable[4] = {};
    const Vlc* orientTable = nullptr;
    const Vlc* dcTable[3] = {};
    std::vector<uint8_t> predictionTable;
    int mbWidth = 0, mbHeight = 0, mbX = 0, mbY = 0;
    int quant = 0, dquant = 0, qsum = 0, useQuantMatrix = 0, loopFilter = 0;
    int quantDcChroma = 0, divideQuantDcLuma = 0, divideQuantDcChroma = 0;
    int edges = 0, flatDc = 0, predictedDc = 0, rawOrient = 0, chromaOrient = 0, orient = 0, estRun = 0;
    uint8_t* dest[3] = {};
    int lines[3] = {};
    uint8_t scratchpad[42] = {};
    alignas(16) int16_t block[64] = {};
};

}
