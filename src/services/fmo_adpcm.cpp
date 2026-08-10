#include "services/fmo_adpcm.h"

size_t FMO_ADPCM_Decode(const uint8_t *input, const size_t input_size,
                        const int16_t initial_sample,
                        const uint8_t initial_index, int16_t *output,
                        const size_t output_capacity)
{
    static const int8_t index_table[16] = {
        -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8,
    };
    static const int16_t step_table[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
        50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
        230,253,279,307,337,371,408,449,494,544,598,658,724,796,
        876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
        2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
        7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
        20350,22385,24623,27086,29794,32767,
    };
    if (input == nullptr || output == nullptr) return 0u;
    int sample = initial_sample;
    int index = initial_index > 88u ? 88 : initial_index;
    size_t count = 0u;
    for (size_t i = 0; i < input_size && count + 2u <= output_capacity; ++i) {
        const uint8_t nibbles[2] = {static_cast<uint8_t>(input[i] >> 4u),
                                    static_cast<uint8_t>(input[i] & 0x0fu)};
        for (const uint8_t nibble : nibbles) {
            const int step = step_table[index];
            int difference = step >> 3;
            if (nibble & 4u) difference += step;
            if (nibble & 2u) difference += step >> 1;
            if (nibble & 1u) difference += step >> 2;
            sample += (nibble & 8u) ? -difference : difference;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            index += index_table[nibble];
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            output[count++] = static_cast<int16_t>(sample);
        }
    }
    return count;
}
