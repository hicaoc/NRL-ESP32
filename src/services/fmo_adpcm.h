#ifndef SRC_SERVICES_FMO_ADPCM_H
#define SRC_SERVICES_FMO_ADPCM_H

#include <stddef.h>
#include <stdint.h>

size_t FMO_ADPCM_Decode(const uint8_t *input, size_t input_size,
                        int16_t initial_sample, uint8_t initial_index,
                        int16_t *output, size_t output_capacity);

#endif
