#include "audio_codec.h"
#include "read_common.h"
#include "mdct.h"
#include "parallel.h"
#include "wma.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace absent {

namespace {

const uint16_t kBandFrequencies[28] = {
    100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150,
    3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 20675, 28575, 41375, 63875,
};

const uint8_t kScaleSymbols[121] = {
    58, 64, 66, 65, 62, 63, 68, 69, 54, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33, 34, 17, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 15, 16, 14, 13, 12,
    11, 10, 0, 9, 8, 7, 6, 5, 4, 55, 70, 3, 2, 1, 35, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87,
    88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103,
    104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 18, 53, 56, 57, 67, 61, 59, 60,
};

const uint8_t kScaleLengths[121] = {
    5, 6, 7, 7, 5, 6, 9, 10, 15, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 13, 13, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 18, 16, 11, 8, 7, 3, 2, 1,
};

const uint8_t kScaleRunSymbols[120] = {
    103, 80, 60, 18, 56, 21, 90, 58, 27, 69, 84, 48, 86, 47, 19, 32,
    78, 5, 28, 53, 9, 31, 38, 10, 88, 25, 105, 118, 23, 82, 98, 110,
    108, 93, 68, 72, 97, 81, 42, 64, 4, 1, 7, 14, 0, 55, 61, 117,
    24, 44, 67, 70, 99, 96, 95, 2, 77, 52, 111, 102, 101, 46, 73, 109,
    51, 92, 30, 11, 66, 15, 16, 116, 65, 57, 59, 115, 12, 35, 17, 41,
    20, 91, 26, 75, 45, 107, 83, 100, 89, 43, 62, 37, 104, 6, 39, 40,
    34, 79, 8, 63, 87, 94, 49, 50, 22, 119, 33, 36, 113, 106, 112, 71,
    85, 74, 76, 114, 29, 54, 13, 3,
};

const uint8_t kScaleRunLengths[120] = {
    7, 11, 11, 10, 10, 12, 12, 11, 11, 12, 15, 15, 14, 13, 10, 9,
    6, 5, 4, 5, 7, 8, 8, 7, 11, 12, 12, 11, 12, 14, 16, 16,
    15, 13, 10, 12, 12, 12, 12, 8, 4, 2, 6, 7, 9, 9, 9, 10,
    12, 12, 12, 16, 18, 21, 21, 21, 21, 21, 21, 20, 17, 15, 15, 15,
    14, 14, 7, 7, 7, 8, 8, 9, 9, 10, 10, 9, 7, 9, 9, 9,
    11, 11, 12, 15, 15, 14, 14, 15, 15, 11, 9, 9, 8, 5, 8, 9,
    9, 7, 6, 6, 12, 14, 14, 13, 11, 10, 9, 9, 11, 12, 13, 15,
    15, 14, 10, 7, 5, 6, 6, 2,
};

const uint8_t kScaleRuns[120] = {
    0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 0, 1, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 0, 1,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 0, 1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 0, 1, 0, 1, 0, 1,
};

const uint8_t kScaleLevels[120] = {
    0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 7, 7, 8, 8, 9, 9,
};

const uint8_t kCoef0Lengths[272] = {
    2, 9, 14, 14, 13, 12, 13, 14, 15, 15, 12, 10, 10, 10, 13, 14,
    15, 15, 12, 11, 13, 14, 14, 13, 15, 15, 14, 12, 12, 8, 10, 10,
    15, 15, 14, 13, 14, 14, 13, 15, 20, 20, 19, 21, 21, 20, 19, 17,
    17, 18, 18, 15, 15, 13, 12, 14, 15, 15, 14, 15, 15, 12, 11, 6,
    7, 8, 9, 13, 13, 13, 14, 14, 11, 10, 7, 8, 14, 14, 14, 14,
    12, 13, 13, 12, 12, 12, 11, 9, 13, 14, 14, 12, 11, 11, 11, 9,
    8, 7, 14, 15, 15, 14, 14, 12, 13, 15, 16, 17, 17, 14, 12, 12,
    12, 15, 15, 14, 14, 14, 13, 13, 9, 9, 11, 11, 10, 7, 6, 13,
    15, 15, 14, 14, 14, 13, 14, 15, 15, 13, 14, 14, 14, 14, 10, 9,
    10, 10, 11, 11, 10, 8, 9, 13, 14, 14, 12, 11, 14, 15, 15, 13,
    12, 14, 14, 14, 14, 13, 14, 14, 3, 5, 8, 10, 10, 15, 15, 14,
    14, 16, 16, 15, 12, 11, 11, 11, 7, 8, 8, 9, 12, 13, 13, 12,
    14, 15, 15, 13, 10, 11, 11, 13, 14, 14, 13, 14, 14, 11, 10, 13,
    15, 15, 14, 12, 11, 4, 6, 6, 8, 12, 12, 12, 13, 13, 12, 13,
    13, 14, 14, 13, 13, 13, 9, 7, 9, 11, 14, 14, 13, 14, 14, 13,
    10, 8, 7, 5, 9, 12, 13, 14, 15, 15, 12, 12, 10, 14, 14, 13,
    12, 13, 14, 14, 12, 13, 13, 12, 12, 12, 9, 7, 6, 3, 4, 4,
};

const uint16_t kCoef0Symbols[272] = {
    2, 25, 111, 94, 69, 58, 87, 93, 136, 135, 59, 37, 34, 36, 82, 182,
    120, 138, 195, 45, 168, 216, 178, 86, 140, 219, 186, 162, 239, 18, 156, 35,
    127, 236, 109, 85, 180, 253, 88, 147, 268, 264, 256, 266, 270, 262, 260, 248,
    246, 252, 258, 137, 189, 230, 64, 179, 146, 208, 101, 118, 238, 163, 46, 9,
    153, 0, 26, 247, 169, 76, 202, 131, 194, 38, 13, 19, 132, 106, 191, 97,
    65, 198, 77, 62, 66, 164, 48, 27, 81, 183, 102, 60, 47, 49, 159, 227,
    20, 14, 112, 263, 144, 217, 104, 63, 79, 209, 269, 250, 254, 203, 241, 196,
    61, 220, 148, 124, 185, 100, 80, 78, 193, 28, 50, 235, 41, 1, 10, 171,
    226, 150, 103, 114, 115, 170, 105, 211, 149, 249, 108, 188, 107, 255, 231, 155,
    42, 40, 55, 160, 39, 21, 29, 215, 234, 184, 228, 51, 116, 142, 145, 172,
    165, 181, 130, 113, 117, 89, 128, 204, 3, 7, 154, 157, 43, 141, 265, 133,
    225, 271, 244, 221, 74, 54, 56, 52, 15, 222, 22, 30, 83, 199, 173, 73,
    123, 210, 143, 175, 44, 53, 237, 174, 139, 134, 110, 218, 129, 161, 213, 177,
    267, 151, 125, 67, 223, 5, 11, 192, 23, 214, 243, 166, 200, 176, 68, 224,
    187, 257, 261, 232, 96, 251, 31, 16, 32, 57, 207, 121, 91, 126, 119, 99,
    158, 24, 212, 8, 33, 70, 92, 205, 240, 242, 75, 197, 233, 259, 190, 98,
    71, 201, 122, 206, 72, 90, 95, 84, 167, 245, 229, 17, 12, 4, 152, 6,
};

const uint8_t kCoef1Symbols[244] = {
    2, 3, 102, 4, 148, 134, 171, 18, 11, 159, 14, 156, 235, 61, 38, 153,
    48, 49, 23, 203, 208, 204, 129, 94, 87, 62, 174, 147, 29, 191, 64, 65,
    146, 164, 142, 132, 103, 154, 165, 181, 109, 30, 86, 92, 239, 138, 39, 50,
    115, 238, 228, 236, 222, 216, 226, 196, 192, 120, 221, 51, 24, 143, 7, 9,
    152, 136, 160, 241, 66, 168, 219, 113, 193, 19, 173, 105, 149, 15, 205, 207,
    125, 190, 182, 68, 70, 67, 137, 31, 223, 116, 210, 220, 198, 126, 88, 41,
    25, 40, 73, 243, 53, 195, 183, 225, 52, 71, 121, 89, 170, 55, 69, 83,
    209, 108, 32, 54, 122, 184, 176, 42, 12, 161, 6, 167, 106, 20, 145, 111,
    43, 26, 175, 107, 34, 33, 197, 74, 128, 232, 212, 224, 202, 90, 57, 227,
    97, 93, 140, 185, 27, 16, 158, 211, 56, 117, 72, 166, 91, 95, 80, 101,
    194, 127, 82, 21, 144, 177, 151, 10, 157, 8, 5, 13, 0, 213, 46, 199,
    35, 162, 135, 169, 45, 59, 114, 44, 188, 186, 75, 79, 118, 187, 112, 139,
    178, 81, 110, 28, 163, 133, 104, 17, 22, 229, 172, 217, 201, 36, 218, 242,
    240, 234, 230, 206, 200, 214, 130, 131, 141, 84, 76, 215, 58, 231, 233, 180,
    77, 37, 189, 179, 155, 47, 96, 99, 119, 63, 237, 78, 85, 60, 98, 100,
    124, 123, 150, 1,
};

const uint8_t kCoef1Lengths[244] = {
    2, 3, 3, 4, 6, 9, 10, 10, 8, 8, 9, 14, 15, 15, 13, 13,
    14, 14, 11, 13, 19, 19, 18, 17, 16, 15, 15, 15, 12, 12, 15, 15,
    14, 13, 5, 4, 5, 7, 9, 11, 12, 12, 16, 16, 15, 14, 13, 14,
    15, 21, 21, 21, 21, 20, 20, 18, 17, 16, 14, 14, 11, 8, 6, 7,
    10, 12, 12, 15, 15, 14, 14, 14, 12, 10, 10, 8, 9, 9, 13, 13,
    17, 17, 16, 15, 15, 15, 13, 12, 14, 15, 19, 19, 18, 17, 16, 13,
    11, 13, 15, 15, 14, 12, 11, 14, 14, 15, 16, 16, 14, 14, 15, 15,
    13, 11, 12, 14, 16, 16, 15, 13, 8, 8, 5, 9, 9, 10, 12, 13,
    13, 11, 10, 10, 12, 12, 12, 15, 17, 20, 20, 19, 18, 16, 14, 14,
    16, 16, 15, 11, 11, 9, 11, 13, 14, 15, 15, 13, 16, 16, 15, 16,
    17, 17, 15, 10, 10, 10, 6, 7, 7, 6, 4, 8, 9, 13, 13, 12,
    12, 12, 10, 9, 13, 14, 14, 13, 16, 16, 15, 15, 15, 11, 13, 14,
    15, 15, 12, 11, 8, 6, 6, 9, 10, 14, 14, 13, 12, 12, 20, 22,
    22, 21, 19, 18, 18, 18, 17, 17, 15, 15, 15, 13, 14, 14, 14, 15,
    15, 12, 11, 10, 10, 13, 16, 16, 15, 14, 14, 15, 15, 14, 16, 16,
    16, 16, 11, 7,
};

const uint16_t kCoef0Runs[272] = {
    0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,
    94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141,
    142, 143, 144, 145, 146, 147, 148, 149, 0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1,
    2, 3, 4, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
};

const float kCoef0Levels[272] = {
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5,
    5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20,
    20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28,
};

const uint16_t kCoef1Runs[244] = {
    0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,
    94, 95, 96, 97, 98, 99, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1,
    2, 3, 4, 5, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 0, 0,
};

const float kCoef1Levels[244] = {
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4,
    4, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9,
    9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17,
    17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25,
    25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 30, 31, 31, 32, 32, 33,
    33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41,
    41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49,
    49, 50, 51, 52,
};

const uint8_t kVec4Lengths[127] = {
    1, 6, 8, 10, 10, 10, 10, 8, 8, 10, 10, 9, 8, 8, 9, 12,
    12, 11, 12, 12, 11, 9, 9, 8, 8, 9, 9, 8, 8, 9, 9, 12,
    12, 12, 14, 14, 13, 11, 11, 9, 8, 9, 9, 11, 11, 10, 9, 8,
    6, 6, 6, 6, 6, 6, 11, 11, 10, 11, 11, 10, 10, 11, 11, 9,
    7, 6, 7, 7, 6, 6, 6, 5, 7, 11, 11, 10, 9, 8, 6, 9,
    9, 10, 10, 9, 8, 8, 6, 6, 6, 8, 8, 9, 12, 12, 11, 10,
    8, 8, 8, 10, 10, 10, 10, 9, 9, 8, 10, 11, 11, 9, 8, 8,
    8, 9, 9, 10, 11, 12, 12, 9, 9, 9, 8, 8, 7, 7, 7,
};

const uint16_t kVec4Symbols[127] = {
    0, 4370, 275, 8195, 4146, 12545, 8225, 290, 4625, 515, 20, 8706, 8210, 4355, 4131, 16385,
    5121, 8961, 321, 1041, 51, 4641, 546, 4610, 530, 513, 8451, 4385, 4130, 33, 8211, 5,
    66, 4161, 1281, 81, 6, 801, 8196, 8481, 8449, 4611, 531, 561, 769, 12290, 8226, 19,
    4097, 2, 4369, 274, 4354, 4114, 12291, 16641, 12305, 49, 12321, 260, 4100, 516, 21, 12546,
    8466, 4353, 4371, 4626, 257, 18, 17, 1, 4386, 8241, 771, 4865, 8705, 8194, 4098, 12561,
    276, 50, 785, 4116, 8209, 4099, 273, 4113, 258, 259, 4609, 35, 1026, 1025, 16401, 305,
    34, 529, 289, 770, 12289, 4, 4145, 4356, 12306, 8193, 12801, 261, 16386, 4881, 3, 514,
    4129, 545, 306, 36, 4101, 65, 20481, 786, 4401, 4866, 8721, 291, 8450, 8465, 4115,
};

const uint8_t kVec2Symbols[137] = {
    19, 165, 211, 46, 75, 177, 12, 86, 83, 38, 133, 178, 28, 104, 73, 35,
    52, 113, 8, 101, 69, 0, 71, 119, 91, 179, 114, 166, 10, 44, 145, 66,
    21, 24, 146, 26, 65, 5, 226, 225, 15, 180, 147, 115, 40, 89, 134, 84,
    54, 42, 60, 31, 193, 181, 76, 148, 37, 67, 33, 3, 17, 2, 102, 87,
    116, 56, 50, 20, 120, 58, 29, 194, 135, 97, 7, 105, 13, 241, 16, 45,
    149, 74, 98, 23, 85, 70, 195, 161, 129, 72, 51, 36, 117, 61, 11, 162,
    1, 4, 49, 68, 9, 27, 130, 39, 53, 99, 25, 150, 90, 103, 163, 196,
    210, 136, 121, 41, 131, 43, 164, 118, 88, 81, 6, 55, 59, 30, 209, 14,
    151, 106, 82, 22, 100, 132, 57, 18, 34,
};

const uint8_t kVec2Lengths[137] = {
    5, 10, 11, 11, 10, 11, 11, 8, 7, 7, 9, 10, 10, 9, 9, 5,
    6, 9, 9, 8, 7, 3, 8, 9, 10, 10, 8, 10, 10, 10, 10, 6,
    6, 8, 9, 9, 7, 7, 11, 12, 12, 10, 9, 8, 8, 9, 9, 7,
    7, 9, 10, 11, 11, 10, 10, 9, 6, 6, 6, 6, 6, 6, 8, 8,
    8, 8, 5, 5, 9, 9, 10, 10, 9, 8, 8, 9, 11, 12, 12, 10,
    9, 9, 7, 7, 7, 7, 10, 10, 9, 8, 5, 5, 8, 10, 10, 9,
    7, 6, 6, 6, 9, 9, 8, 7, 6, 7, 8, 9, 9, 8, 9, 10,
    10, 9, 9, 8, 8, 9, 9, 8, 8, 7, 7, 7, 9, 10, 11, 11,
    9, 9, 6, 6, 7, 8, 8, 4, 4,
};

const uint8_t kVec1Symbols[101] = {
    7, 32, 59, 60, 83, 82, 62, 33, 45, 61, 84, 85, 1, 13, 19, 25,
    34, 46, 47, 14, 6, 64, 87, 86, 63, 88, 90, 35, 26, 0, 48, 65,
    66, 36, 15, 20, 91, 89, 67, 49, 50, 69, 92, 93, 27, 5, 37, 68,
    71, 51, 52, 70, 94, 96, 38, 21, 16, 4, 28, 53, 95, 97, 73, 39,
    29, 72, 98, 99, 54, 40, 22, 30, 55, 74, 76, 56, 75, 77, 17, 3,
    23, 41, 57, 78, 79, 31, 10, 9, 100, 2, 11, 8, 18, 42, 58, 80,
    81, 43, 44, 24, 12,
};

const uint8_t kVec1Lengths[101] = {
    5, 8, 10, 10, 11, 11, 10, 8, 9, 10, 11, 11, 6, 5, 6, 7,
    8, 9, 9, 5, 5, 10, 11, 11, 10, 11, 11, 8, 7, 7, 9, 10,
    10, 8, 5, 6, 11, 11, 10, 9, 9, 10, 11, 11, 7, 5, 8, 10,
    10, 9, 9, 10, 11, 11, 8, 6, 5, 5, 7, 9, 11, 11, 10, 8,
    7, 10, 11, 11, 9, 8, 6, 7, 9, 10, 10, 9, 10, 10, 5, 5,
    6, 8, 9, 10, 10, 7, 4, 4, 5, 5, 4, 4, 5, 8, 9, 10,
    10, 8, 8, 6, 4,
};

const float kDecorrelation[91] = {
    1.000000, 0.707031, -0.707031, 0.707031, 0.707031, 0.578125, 0.707031, 0.410156, 0.578125, -0.707031, 0.410156, 0.578125, 0.000000, -0.816406, 0.500000, 0.652344,
    0.500000, 0.269531, 0.500000, 0.269531, -0.500000, -0.652344, 0.500000, -0.269531, -0.500000, 0.652344, 0.500000, -0.652344, 0.500000, -0.269531, 0.445312, 0.601562,
    0.511719, 0.371094, 0.195312, 0.445312, 0.371094, -0.195312, -0.601562, -0.511719, 0.445312, 0.000000, -0.632812, 0.000000, 0.632812, 0.445312, -0.371094, -0.195312,
    0.601562, -0.511719, 0.445312, -0.601562, 0.511719, -0.371094, 0.195312, 0.410156, 0.558594, 0.500000, 0.410156, 0.289062, 0.148438, 0.410156, 0.410156, 0.000000,
    -0.410156, -0.578125, -0.410156, 0.410156, 0.148438, -0.500000, -0.410156, 0.289062, 0.558594, 0.410156, -0.148438, -0.500000, 0.410156, 0.289062, -0.558594, 0.410156,
    -0.410156, 0.000000, 0.410156, -0.578125, 0.410156, 0.410156, -0.558594, 0.500000, -0.410156, 0.289062, -0.148438,
};

const int kMaxChannels = 8;
const int kMaxSubframes = 32;
const int kMaxBands = 29;
const int kMaxFrameBytes = 32768;
const int kMinBits = 6;
const int kMaxBits = 13;
const int kMostOutput = 1 << 24;

struct Codes {
    Vlc scale, scaleRun, vec4, vec2, vec1;
    wma::RunLevel coef[2];
    float sin64[33];
};

const Codes& codes() {
    static const Codes made = [] {
        Codes c;
        c.scale.buildInOrder(8, 121, kScaleLengths, kScaleSymbols, -60);
        c.scaleRun.buildInOrder(9, 120, kScaleRunLengths, kScaleRunSymbols, 0);
        c.vec4.buildInOrder(9, 127, kVec4Lengths, kVec4Symbols, -1);
        c.vec2.buildInOrder(9, 137, kVec2Lengths, kVec2Symbols, -1);
        c.vec1.buildInOrder(9, 101, kVec1Lengths, kVec1Symbols, 0);
        c.coef[0].vlc.buildInOrder(9, 272, kCoef0Lengths, kCoef0Symbols, 0);
        c.coef[0].levels.assign(kCoef0Levels, kCoef0Levels + 272);
        c.coef[0].runs.assign(kCoef0Runs, kCoef0Runs + 272);
        c.coef[1].vlc.buildInOrder(9, 244, kCoef1Lengths, kCoef1Symbols, 0);
        c.coef[1].levels.assign(kCoef1Levels, kCoef1Levels + 244);
        c.coef[1].runs.assign(kCoef1Runs, kCoef1Runs + 244);
        for (int i = 0; i < 33; ++i) c.sin64[i] = float(sin(i * M_PI / 64.0));
        return c;
    }();
    return made;
}

int log2Of(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }

struct Channel {
    int prevBlockLength = 0;
    bool transmit = false;
    int subframes = 0;
    int lengths[kMaxSubframes + 1] = {};
    int current = 0;
    int decoded = 0;
    bool grouped = false;
    int quantStep = 0;
    bool reuse = false;
    int scaleStep = 0;
    int maxScale = 0;
    int saved[2][kMaxBands] = {};
    int savedIndex = 0;
    int scalesAt = 0;
    int tableIndex = 0;
    int coefsAt = 0;
    int vectorCoefs = 0;
};

struct Group {
    int count = 0;
    bool transform = false;
    int8_t bands[kMaxBands] = {};
    float matrix[kMaxChannels * kMaxChannels] = {};
    int members[kMaxChannels] = {};
};

struct WmaPro : PacketDecoder {
    int blockAlign = 0;
    int bitsPerSample = 16;
    unsigned decodeFlags = 0;
    bool lengthPrefix = false, dynamicRange = false;
    int frameLength = 0, frameBits = 0, log2FrameSize = 0;
    int maxSubframes = 1, subframeLengthBits = 0, minSubframe = 0, blockSizes = 1;
    bool maxSubframeBit = false;
    int bandCount[kMaxBits - kMinBits + 1] = {};
    int16_t bandOffsets[kMaxBits - kMinBits + 1][kMaxBands] = {};
    int8_t scaleOffsets[kMaxBits - kMinBits + 1][kMaxBits - kMinBits + 1][kMaxBands] = {};
    std::vector<InverseMdct> mdct;
    int mdctFirst = 0;

    uint8_t stored[kMaxFrameBytes + 64] = {};
    int64_t putCount = 0;
    int64_t savedBits = 0;
    int frameOffset = 0;
    bool packetLoss = true, packetDone = false, skipFrame = true;
    int sequence = 0;
    size_t nextPacketStart = 0;
    int packetOffset = 0;
    int trimStart = 0, trimEnd = 0;
    BitReader gb, pgb;
    int64_t bufferBits = 0;

    bool parsedAll = false;
    int subframeLength = 0;
    int forSubframe = 0;
    int indexes[kMaxChannels] = {};
    int bandsNow = 0;
    bool transmitVectors = false;
    int tableIndex = 0;
    int escapeLength = 0;
    int groupCount = 0;
    Group groups[kMaxChannels];
    Channel ch[kMaxChannels];
    std::vector<float> out[kMaxChannels];
    float tmp[1 << kMaxBits] = {};

    bool quiet = false;
    std::vector<float>* sink = nullptr;
    int64_t made = 0;
    int64_t frameAt = -1;
    bool frameWhole = false;
    int framesThisBlock = 0;

    bool setup(const std::vector<uint8_t>& w, std::string* error) {
        auto fail = [&](const char* text) {
            if (error) *error = text;
            return false;
        };
        const int declared = int(le16(w.data() + 2));
        rate = int(le32(w.data() + 4));
        blockAlign = int(le16(w.data() + 12));
        size_t extra = w.size() >= 18 ? std::min<size_t>(le16(w.data() + 16), w.size() - 18) : 0;
        const uint8_t* e = w.data() + 18;
        if (le16(w.data()) == 0xFFFE && extra >= 22) {
            e += 22;
            extra -= 22;
        }
        if (!blockAlign) return fail("Windows Media Audio track without a block size");
        if (extra < 18) return fail("Windows Media Audio 9 Professional track without its codec data");
        if (rate < 1 || rate > 768000) return fail("Windows Media Audio layout not supported");
        decodeFlags = le16(e + 14);
        const uint32_t mask = le32(e + 2);
        bitsPerSample = int(le16(e));
        channels = mask ? __builtin_popcount(mask) : declared;
        if (bitsPerSample > 32 || bitsPerSample < 1) return fail("Windows Media Audio 9 Professional sample size not supported");
        log2FrameSize = log2Of(unsigned(blockAlign)) + 4;
        lengthPrefix = decodeFlags & 0x40;
        frameBits = wma::frameLengthBits(rate, 3, decodeFlags);
        if (frameBits > kMaxBits) return fail("Windows Media Audio 9 Professional frames of 16384 samples are not supported");
        frameLength = 1 << frameBits;
        const int log2Subframes = int((decodeFlags & 0x38) >> 3);
        maxSubframes = 1 << log2Subframes;
        maxSubframeBit = maxSubframes == 16 || maxSubframes == 4;
        subframeLengthBits = log2Of(unsigned(log2Subframes)) + 1;
        blockSizes = log2Subframes + 1;
        minSubframe = frameLength / maxSubframes;
        dynamicRange = decodeFlags & 0x80;
        if (maxSubframes > kMaxSubframes || minSubframe < (1 << kMinBits) || channels <= 0 || channels > kMaxChannels || channels > declared)
            return fail("Windows Media Audio 9 Professional layout not supported");
        for (int i = 0; i < blockSizes; ++i) {
            const int length = frameLength >> i;
            int band = 1;
            bandOffsets[i][0] = 0;
            for (int x = 0; x < kMaxBands - 1 && bandOffsets[i][band - 1] < length; ++x) {
                int offset = (length * 2 * kBandFrequencies[x]) / rate + 2;
                offset &= ~3;
                if (offset > bandOffsets[i][band - 1]) bandOffsets[i][band++] = int16_t(offset);
                if (offset >= length) break;
            }
            bandOffsets[i][band - 1] = int16_t(length);
            bandCount[i] = band - 1;
            if (bandCount[i] <= 0) return fail("Windows Media Audio 9 Professional layout not supported");
        }
        for (int i = 0; i < blockSizes; ++i) {
            for (int b = 0; b < bandCount[i]; ++b) {
                const int offset = ((bandOffsets[i][b] + bandOffsets[i][b + 1] - 1) << i) >> 1;
                for (int x = 0; x < blockSizes; ++x) {
                    int v = 0;
                    while (v + 1 < kMaxBands && bandOffsets[x][v + 1] << x < offset) ++v;
                    scaleOffsets[i][x][b] = int8_t(v);
                }
            }
        }
        mdctFirst = frameBits - blockSizes + 1;
        for (int bits = mdctFirst; bits <= frameBits; ++bits) {
            const float scale = float(1.0 / (1 << (bits - 1)) / double(1LL << (bitsPerSample - 1)));
            mdct.emplace_back(bits, double(scale));
        }
        for (int c = 0; c < channels; ++c) out[c].assign(size_t(frameLength + frameLength / 2), 0.0f);
        reset();
        return true;
    }

    void reset() override {
        for (int c = 0; c < channels; ++c) {
            std::fill(out[c].begin(), out[c].end(), 0.0f);
            ch[c] = Channel();
            ch[c].prevBlockLength = frameLength;
            groups[c] = Group();
        }
        putCount = savedBits = 0;
        frameOffset = packetOffset = sequence = 0;
        packetLoss = true;
        packetDone = false;
        skipFrame = true;
        trimStart = trimEnd = 0;
    }

    float* coefs(int c) { return out[c].data() + ch[c].coefsAt; }

    int subframeLengthAt(int offset) {
        if (offset == frameLength - minSubframe) return minSubframe;
        if (gb.left() < 1) return -1;
        int shift = 0;
        if (maxSubframeBit) {
            if (gb.bit()) shift = 1 + int(gb.read(subframeLengthBits - 1));
        } else {
            shift = int(gb.read(subframeLengthBits));
        }
        const int length = frameLength >> shift;
        if (length < minSubframe || length > frameLength) return -1;
        return length;
    }

    bool tiles() {
        int samples[kMaxChannels] = {};
        bool contains[kMaxChannels] = {};
        int forCurrent = channels;
        bool fixed = false;
        int least = 0;
        for (int c = 0; c < channels; ++c) ch[c].subframes = 0;
        if (maxSubframes == 1 || gb.bit()) fixed = true;
        do {
            for (int c = 0; c < channels; ++c) {
                if (samples[c] == least) contains[c] = fixed || forCurrent == 1 || least == frameLength - minSubframe ? true : gb.bit();
                else contains[c] = false;
            }
            const int length = subframeLengthAt(least);
            if (length <= 0) return false;
            least += length;
            for (int c = 0; c < channels; ++c) {
                Channel& h = ch[c];
                if (contains[c]) {
                    if (h.subframes >= kMaxSubframes) return false;
                    h.lengths[h.subframes++] = length;
                    samples[c] += length;
                    if (samples[c] > frameLength) return false;
                } else if (samples[c] <= least) {
                    if (samples[c] < least) {
                        forCurrent = 0;
                        least = samples[c];
                    }
                    ++forCurrent;
                }
            }
        } while (least < frameLength);
        return true;
    }

    void decorrelation(Group& g) {
        const int n = g.count;
        int8_t rotation[kMaxChannels * kMaxChannels];
        std::fill(g.matrix, g.matrix + channels * channels, 0.0f);
        for (int i = 0; i < n * (n - 1) >> 1; ++i) rotation[i] = int8_t(gb.read(6));
        for (int i = 0; i < n; ++i) g.matrix[n * i + i] = gb.bit() ? 1.0f : -1.0f;
        const float* sin64 = codes().sin64;
        int offset = 0;
        for (int i = 1; i < n; ++i) {
            for (int x = 0; x < i; ++x) {
                for (int y = 0; y < i + 1; ++y) {
                    const float v1 = g.matrix[x * n + y];
                    const float v2 = g.matrix[i * n + y];
                    const int k = rotation[offset + x];
                    float sinv, cosv;
                    if (k < 32) {
                        sinv = sin64[k];
                        cosv = sin64[32 - k];
                    } else {
                        sinv = sin64[64 - k];
                        cosv = -sin64[k - 32];
                    }
                    g.matrix[y + x * n] = (v1 * sinv) - (v2 * cosv);
                    g.matrix[y + i * n] = (v1 * cosv) + (v2 * sinv);
                }
            }
            offset += i;
        }
    }

    bool channelTransform() {
        groupCount = 0;
        if (channels <= 1) return true;
        int remaining = forSubframe;
        if (gb.bit()) return false;
        for (groupCount = 0; remaining && groupCount < forSubframe; ++groupCount) {
            Group& g = groups[groupCount];
            g.count = 0;
            g.transform = false;
            if (remaining > 2) {
                for (int i = 0; i < forSubframe; ++i) {
                    const int c = indexes[i];
                    if (!ch[c].grouped && gb.bit()) {
                        g.members[g.count++] = c;
                        ch[c].grouped = true;
                    }
                }
            } else {
                int k = 0;
                for (int i = 0; i < forSubframe; ++i) {
                    const int c = indexes[i];
                    if (!ch[c].grouped) g.members[k++] = c;
                    ch[c].grouped = true;
                }
                g.count = remaining;
            }
            if (g.count == 2) {
                if (gb.bit()) {
                    if (gb.bit()) return false;
                } else {
                    g.transform = true;
                    const float a = channels == 2 ? 1.0f : 0.70703125f;
                    g.matrix[0] = a;
                    g.matrix[1] = -a;
                    g.matrix[2] = a;
                    g.matrix[3] = a;
                }
            } else if (g.count > 2) {
                if (gb.bit()) {
                    g.transform = true;
                    if (gb.bit()) {
                        decorrelation(g);
                    } else if (g.count <= 6) {
                        static const int at[7] = {0, 0, 1, 5, 14, 30, 55};
                        std::copy(kDecorrelation + at[g.count], kDecorrelation + at[g.count] + g.count * g.count, g.matrix);
                    }
                }
            }
            if (g.transform) {
                if (!gb.bit()) {
                    for (int i = 0; i < bandsNow; ++i) g.bands[i] = int8_t(gb.bit());
                } else {
                    std::fill(g.bands, g.bands + bandsNow, int8_t(1));
                }
            }
            remaining -= g.count;
        }
        return true;
    }

    void coefficients(int c) {
        const Codes& k = codes();
        float* to = coefs(c);
        BitReader r = gb;
        const wma::RunLevel& table = k.coef[r.bit()];
        const int limit = subframeLength >> 8, vectors = ch[c].vectorCoefs;
        bool runMode = false;
        int at = 0, zeros = 0;
        while ((transmitVectors || !runMode) && at + 3 < vectors) {
            uint32_t vals[4];
            const int idx = k.vec4.read(r);
            if (idx < 0) {
                for (int i = 0; i < 4; i += 2) {
                    const int pair = k.vec2.read(r);
                    if (pair < 0) {
                        vals[i] = uint32_t(k.vec1.read(r));
                        if (vals[i] == 100) vals[i] += wma::largeValue(r);
                        vals[i + 1] = uint32_t(k.vec1.read(r));
                        if (vals[i + 1] == 100) vals[i + 1] += wma::largeValue(r);
                    } else {
                        vals[i] = uint32_t(pair >> 4);
                        vals[i + 1] = uint32_t(pair & 15);
                    }
                }
            } else {
                vals[0] = uint32_t(idx >> 12);
                vals[1] = uint32_t((idx >> 8) & 15);
                vals[2] = uint32_t((idx >> 4) & 15);
                vals[3] = uint32_t(idx & 15);
            }
            for (int i = 0; i < 4; ++i, ++at) {
                if (vals[i]) {
                    const float v = float(vals[i]);
                    to[at] = r.bit() ? v : -v;
                    zeros = 0;
                } else {
                    to[at] = 0;
                    runMode |= ++zeros > limit;
                }
            }
        }
        if (at < subframeLength) {
            std::fill(to + at, to + subframeLength, 0.0f);
            wma::runLevel(r, table, 1, to, at, subframeLength, subframeLength, escapeLength, 0);
        }
        gb = r;
    }

    bool scaleFactors() {
        const Codes& k = codes();
        for (int i = 0; i < forSubframe; ++i) {
            Channel& h = ch[indexes[i]];
            h.scalesAt = !h.savedIndex;
            int* sf = h.saved[h.scalesAt];
            if (h.reuse) {
                const int8_t* from = scaleOffsets[tableIndex][h.tableIndex];
                for (int b = 0; b < bandsNow; ++b) sf[b] = h.saved[h.savedIndex][from[b]];
            }
            if (!h.current || gb.bit()) {
                if (!h.reuse) {
                    h.scaleStep = int(gb.read(2)) + 1;
                    int val = 45 / h.scaleStep;
                    for (int b = 0; b < bandsNow; ++b) {
                        val += k.scale.read(gb);
                        sf[b] = val;
                    }
                } else {
                    for (int b = 0; b < bandsNow; ++b) {
                        const int idx = k.scaleRun.read(gb);
                        int skip, val, sign;
                        if (!idx) {
                            const uint32_t code = gb.read(14);
                            val = int(code >> 6);
                            sign = int(code & 1) - 1;
                            skip = int(code & 0x3f) >> 1;
                        } else if (idx == 1) {
                            break;
                        } else if (idx < 0) {
                            return false;
                        } else {
                            skip = kScaleRuns[idx];
                            val = kScaleLevels[idx];
                            sign = int(gb.read(1)) - 1;
                        }
                        b += skip;
                        if (b >= bandsNow) return false;
                        sf[b] += (val ^ sign) - sign;
                    }
                }
                h.savedIndex = !h.savedIndex;
                h.tableIndex = tableIndex;
                h.reuse = true;
            }
            h.maxScale = sf[0];
            for (int b = 1; b < bandsNow; ++b) h.maxScale = std::max(h.maxScale, sf[b]);
        }
        return true;
    }

    void inverseTransform() {
        const int16_t* sfb = bandOffsets[tableIndex];
        for (int i = 0; i < groupCount; ++i) {
            Group& g = groups[i];
            if (!g.transform) continue;
            const int n = g.count;
            float* data[kMaxChannels];
            for (int k = 0; k < n; ++k) data[k] = coefs(g.members[k]);
            for (int b = 0; b < bandsNow; ++b) {
                const int end = std::min<int>(sfb[b + 1], subframeLength);
                if (g.bands[b] == 1) {
                    for (int y = sfb[b]; y < end; ++y) {
                        float v[kMaxChannels];
                        for (int k = 0; k < n; ++k) v[k] = data[k][y];
                        const float* m = g.matrix;
                        for (int k = 0; k < n; ++k) {
                            float sum = 0;
                            for (int j = 0; j < n; ++j) sum += v[j] * *m++;
                            data[k][y] = sum;
                        }
                    }
                } else if (channels == 2) {
                    const float scale = float(181.0 / 128);
                    for (int y = sfb[b]; y < end; ++y) {
                        data[0][y] = data[0][y] * scale;
                        data[1][y] = data[1][y] * scale;
                    }
                }
            }
        }
    }

    void window() {
        for (int i = 0; i < forSubframe; ++i) {
            const int c = indexes[i];
            int length = ch[c].prevBlockLength;
            ch[c].prevBlockLength = subframeLength;
            if (quiet) continue;
            float* start = coefs(c) - (length >> 1);
            if (subframeLength < length) {
                start += (length - subframeLength) >> 1;
                length = subframeLength;
            }
            const float* w = sineWindow(log2Of(unsigned(length)));
            const int half = length >> 1;
            float* dst = start + half;
            const float* src0 = start + half;
            const float* src1 = start + half;
            const float* win = w + half;
            for (int a = -half, b = half - 1; a < 0; ++a, --b) {
                const float s0 = src0[a], s1 = src1[b], wa = win[a], wb = win[b];
                dst[a] = s0 * wb - s1 * wa;
                dst[b] = s0 * wa + s1 * wb;
            }
        }
    }

    bool subframe() {
        int offset = frameLength, length = frameLength;
        for (int c = 0; c < channels; ++c) {
            ch[c].grouped = false;
            if (offset > ch[c].decoded) {
                offset = ch[c].decoded;
                length = ch[c].lengths[ch[c].current];
            }
        }
        forSubframe = 0;
        int total = frameLength * channels;
        for (int c = 0; c < channels; ++c) {
            Channel& h = ch[c];
            total -= h.decoded;
            if (offset == h.decoded && length == h.lengths[h.current]) {
                total -= h.lengths[h.current];
                h.decoded += h.lengths[h.current];
                indexes[forSubframe++] = c;
            }
        }
        if (!total) parsedAll = true;
        if (length < minSubframe || length > frameLength) return false;
        tableIndex = log2Of(unsigned(frameLength / length));
        bandsNow = bandCount[tableIndex];
        offset += frameLength >> 1;
        for (int i = 0; i < forSubframe; ++i) ch[indexes[i]].coefsAt = offset;
        subframeLength = length;
        escapeLength = log2Of(unsigned(length - 1)) + 1;
        if (gb.bit()) {
            int fill = int(gb.read(2));
            if (!fill) {
                const int n = int(gb.read(4));
                fill = int(gb.read(n)) + 1;
            }
            if (gb.position() + fill > savedBits) return false;
            gb.skip(fill);
        }
        if (gb.bit()) return false;
        if (!channelTransform()) return false;
        bool any = false;
        for (int i = 0; i < forSubframe; ++i) {
            if ((ch[indexes[i]].transmit = gb.bit())) any = true;
        }
        if (any) {
            int quant = 90 * bitsPerSample >> 4;
            if ((transmitVectors = gb.bit())) {
                const int bits = log2Of(unsigned((length + 3) / 4)) + 1;
                for (int i = 0; i < forSubframe; ++i) {
                    const int n = int(gb.read(bits)) << 2;
                    if (n > length) return false;
                    ch[indexes[i]].vectorCoefs = n;
                }
            } else {
                for (int i = 0; i < forSubframe; ++i) ch[indexes[i]].vectorCoefs = length;
            }
            int step = int(gb.read(6));
            if (step & 32) step -= 64;
            quant += step;
            if (step == -32 || step == 31) {
                const int sign = (step == 31) - 1;
                int more = 0;
                while (gb.position() + 5 < savedBits && (step = int(gb.read(5))) == 31) more += 31;
                quant += ((more + step) ^ sign) - sign;
            }
            if (forSubframe == 1) {
                ch[indexes[0]].quantStep = quant;
            } else {
                const int modifier = int(gb.read(3));
                for (int i = 0; i < forSubframe; ++i) {
                    Channel& h = ch[indexes[i]];
                    h.quantStep = quant;
                    if (gb.bit()) h.quantStep += modifier ? int(gb.read(modifier)) + 1 : 1;
                }
            }
            if (!scaleFactors()) return false;
        }
        for (int i = 0; i < forSubframe; ++i) {
            const int c = indexes[i];
            if (ch[c].transmit && gb.position() < savedBits) coefficients(c);
            else std::fill(coefs(c), coefs(c) + length, 0.0f);
        }
        if (any && !quiet) {
            InverseMdct& transform = mdct[size_t(log2Of(unsigned(length)) - mdctFirst)];
            inverseTransform();
            const int16_t* sfb = bandOffsets[tableIndex];
            for (int i = 0; i < forSubframe; ++i) {
                const int c = indexes[i];
                const Channel& h = ch[c];
                const int* sf = h.saved[h.scalesAt];
                float* from = coefs(c);
                for (int b = 0; b < bandsNow; ++b) {
                    const int end = std::min<int>(sfb[b + 1], length);
                    const int power = h.quantStep - (h.maxScale - sf[b]) * h.scaleStep;
                    const float quant = float(exp2(3.32192809488736234787 * (power / 20.0)));
                    for (int y = sfb[b]; y < end; ++y) tmp[y] = from[y] * quant;
                }
                transform.half(tmp, from);
            }
        }
        window();
        for (int i = 0; i < forSubframe; ++i) {
            Channel& h = ch[indexes[i]];
            if (h.current >= h.subframes) return false;
            ++h.current;
        }
        return true;
    }

    int frame() {
        int length = 0;
        if (lengthPrefix) length = int(gb.read(log2FrameSize));
        if (!tiles()) {
            packetLoss = true;
            return 0;
        }
        if (channels > 1 && gb.bit()) {
            if (gb.bit()) gb.skip(4 * channels * channels);
        }
        if (dynamicRange) gb.skip(8);
        if (gb.bit()) {
            if (gb.bit()) trimStart = int(gb.read(log2Of(unsigned(frameLength * 2))));
            if (gb.bit()) trimEnd = int(gb.read(log2Of(unsigned(frameLength * 2))));
        } else {
            trimStart = trimEnd = 0;
        }
        parsedAll = false;
        for (int c = 0; c < channels; ++c) {
            ch[c].decoded = 0;
            ch[c].current = 0;
            ch[c].reuse = false;
        }
        while (!parsedAll) {
            if (!subframe()) {
                packetLoss = true;
                return 0;
            }
        }
        frameAt = quiet ? 0 : int64_t(sink->size());
        frameWhole = skipFrame;
        skipFrame = false;
        made += frameLength;
        ++framesThisBlock;
        if (!quiet) {
            sink->resize(sink->size() + size_t(frameLength) * size_t(channels));
            float* to = sink->data() + frameAt;
            for (int c = 0; c < channels; ++c) {
                float* from = out[c].data();
                for (int i = 0; i < frameLength; ++i) to[size_t(i) * size_t(channels) + size_t(c)] = from[i];
                memmove(from, from + frameLength, size_t(frameLength / 2) * sizeof(float));
            }
        }
        if (lengthPrefix) {
            if (length != (gb.position() - frameOffset) + 2) {
                packetLoss = true;
                return 0;
            }
            gb.skip(length - (gb.position() - frameOffset) - 1);
        } else {
            while (gb.position() < savedBits && !gb.bit()) {
            }
        }
        return int(gb.bit());
    }

    void write(uint32_t v, int n) {
        while (n > 0) {
            const size_t byte = size_t(putCount >> 3);
            const int free = 8 - int(putCount & 7);
            if (free == 8) stored[byte] = 0;
            const int take = std::min(free, n);
            stored[byte] |= uint8_t(((v >> (n - take)) & ((1u << take) - 1)) << (free - take));
            putCount += take;
            n -= take;
        }
    }

    void copy(const uint8_t* data, size_t size, int64_t at, int64_t n) {
        if (!(at & 7) && size_t((at + n + 7) >> 3) <= size) {
            const uint8_t* src = data + (at >> 3);
            uint8_t* dst = stored + (putCount >> 3);
            const int64_t whole = n >> 3;
            const int r = int(putCount & 7);
            if (!r) {
                memcpy(dst, src, size_t(whole));
            } else {
                for (int64_t i = 0; i < whole; ++i) {
                    dst[i] = uint8_t((dst[i] & (0xFF00 >> r)) | (src[i] >> r));
                    dst[i + 1] = uint8_t(src[i] << (8 - r));
                }
            }
            putCount += whole * 8;
            if (n & 7) write(uint32_t(src[whole] >> (8 - (n & 7))), int(n & 7));
            return;
        }
        BitReader from(data, size);
        from.skip(at);
        for (; n >= 24; n -= 24) write(from.read(24), 24);
        if (n > 0) write(from.read(int(n)), int(n));
    }

    void saveBits(const uint8_t* data, size_t size, int64_t length, bool append) {
        int64_t bytes;
        if (!append) {
            frameOffset = int(pgb.position() & 7);
            savedBits = frameOffset;
            putCount = 0;
            bytes = (savedBits + length + 7) >> 3;
        } else {
            bytes = (putCount + length + 7) >> 3;
        }
        if (length <= 0 || bytes > kMaxFrameBytes) {
            packetLoss = true;
            return;
        }
        savedBits += length;
        if (!append) {
            copy(data, size, pgb.position() & ~int64_t(7), savedBits);
        } else {
            const int align = int(std::min<int64_t>(8 - (pgb.position() & 7), length));
            write(pgb.read(align), align);
            length -= align;
            copy(data, size, pgb.position(), length);
        }
        pgb.skip(length);
        gb = BitReader(stored, sizeof stored, savedBits);
        gb.skip(frameOffset);
    }

    int64_t call(const uint8_t* data, size_t size) {
        frameAt = -1;
        int64_t remaining;
        size_t span;
        if (packetDone || packetLoss) {
            packetDone = false;
            if (size < size_t(blockAlign)) {
                packetLoss = true;
                return -1;
            }
            nextPacketStart = size - size_t(blockAlign);
            span = size_t(blockAlign);
            bufferBits = int64_t(span) * 8;
            pgb = BitReader(data, span);
            framesThisBlock = 0;
            const int number = int(pgb.read(4));
            pgb.skip(2);
            int64_t previous = pgb.read(log2FrameSize);
            if (!packetLoss && ((sequence + 1) & 15) != number) packetLoss = true;
            sequence = number;
            if (previous > 0) {
                remaining = bufferBits - pgb.position();
                if (previous >= remaining) {
                    previous = remaining;
                    packetDone = true;
                }
                saveBits(data, span, previous, true);
                if (!packetLoss) frame();
            }
            if (packetLoss) {
                savedBits = 0;
                packetLoss = false;
            }
        } else {
            if (size < nextPacketStart) {
                packetLoss = true;
                return -1;
            }
            span = size - nextPacketStart;
            bufferBits = int64_t(span) * 8;
            pgb = BitReader(data, span);
            pgb.skip(packetOffset);
            remaining = bufferBits - pgb.position();
            int64_t frameSize = 0;
            if (framesThisBlock * int64_t(frameLength) * channels >= kMostOutput) {
                packetLoss = true;
            } else if (lengthPrefix && remaining > log2FrameSize && (frameSize = pgb.peek(log2FrameSize)) && frameSize <= remaining) {
                saveBits(data, span, frameSize, false);
                if (!packetLoss) packetDone = !frame();
            } else if (!lengthPrefix && savedBits > gb.position()) {
                packetDone = !frame();
            } else {
                packetDone = true;
            }
        }
        remaining = bufferBits - pgb.position();
        if (remaining < 0) packetLoss = true;
        if (packetDone && !packetLoss && remaining > 0) saveBits(data, span, remaining, false);
        packetOffset = int(pgb.position() & 7);
        if (packetLoss) return -1;
        if (trimStart) {
            if (frameAt >= 0 && !frameWhole) {
                if (trimStart < frameLength) {
                    made -= trimStart;
                    if (!quiet) sink->erase(sink->begin() + frameAt, sink->begin() + frameAt + int64_t(trimStart) * channels);
                } else {
                    made -= frameLength;
                    if (!quiet) sink->resize(size_t(frameAt));
                    frameAt = -1;
                }
            }
            trimStart = 0;
        }
        if (trimEnd) {
            if (frameAt >= 0 && !frameWhole) {
                const int64_t have = quiet ? frameLength : int64_t(sink->size() - size_t(frameAt)) / channels;
                const int64_t cut = trimEnd < have ? trimEnd : have;
                made -= cut;
                if (!quiet) sink->resize(sink->size() - size_t(cut) * size_t(channels));
            }
            trimEnd = 0;
        }
        return pgb.position() >> 3;
    }

    void run(const std::vector<uint8_t>& packet) {
        const uint8_t* data = packet.data();
        size_t size = packet.size();
        for (int turn = 0; size > 0 && turn < (1 << 20); ++turn) {
            const int64_t used = call(data, size);
            if (used < 0 || size_t(used) >= size) return;
            data += used;
            size -= size_t(used);
        }
    }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& to) override {
        sink = &to;
        run(packet);
        sink = nullptr;
    }

    void skim(const std::vector<uint8_t>& packet) override {
        quiet = true;
        run(packet);
        quiet = false;
    }

    bool counts(Reader& file, const Track& track, std::vector<int64_t>& frames) override {
        const size_t n = track.samples.size();
        frames.assign(n, 0);
        std::vector<std::vector<uint8_t>> batch;
        for (size_t from = 0; from < n;) {
            const size_t first = from ? from - 1 : 0;
            size_t to = from, bytes = 0;
            while (to < n && (to == from || bytes < (8u << 20))) bytes += track.samples[to++].size;
            batch.resize(to - first);
            for (size_t i = first; i < to; ++i) {
                std::vector<uint8_t>& packet = batch[i - first];
                packet.resize(track.samples[i].size);
                if (!file.read(track, uint32_t(i), packet.data())) packet.clear();
            }
            const size_t parts = std::min(to - from, parallelWidth() * 4);
            parallelFor(parts, [&](size_t part) {
                const size_t start = from + (to - from) * part / parts, end = from + (to - from) * (part + 1) / parts;
                auto look = std::make_unique<WmaPro>(*this);
                look->quiet = true;
                look->skipFrame = start == 0;
                for (size_t i = start ? start - 1 : 0; i < end; ++i) {
                    look->made = 0;
                    if (!batch[i - first].empty()) look->run(batch[i - first]);
                    if (i >= start) frames[i] = look->made;
                }
            });
            from = to;
        }
        const int64_t most = n ? *std::max_element(frames.begin(), frames.end()) : 0;
        prerollFrames = int(std::min<int64_t>(most, 1 << 20)) + 2 * frameLength;
        return true;
    }
};

}

std::unique_ptr<PacketDecoder> openWmaProAudio(const Track& track, std::string* error) {
    const std::vector<uint8_t>& w = track.config;
    if (track.configType != fourcc("wfex") || w.size() < 16) {
        if (error) *error = "Windows Media Audio track without its format header";
        return nullptr;
    }
    auto audio = std::make_unique<WmaPro>();
    if (!audio->setup(w, error)) return nullptr;
    audio->prerollPackets = 3;
    audio->prerollFrames = 2 * audio->frameLength;
    audio->skip = audio->frameLength;
    audio->longestPacket = audio->frameLength;
    return audio;
}

}
