
/*
 * Validate Reed-Solomon CRC and CRC32C routines pass
 * basic sanity check at compile time
 *
 * Shamelessly lifted/copied from CASTOR utils/CRCtest.cpp
 *
 * Designed to abort if CRC32C or RS-CRC fails basic sanity check
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>

uint32_t crc32c(uint32_t seed, const uint8_t *buf, size_t sz);
uint32_t GenerateRSCRC(uint32_t seed, int sz, const uint8_t *buf);

int main(int argc, char *argv[])
{
	const uint8_t block1[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43,
    47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127,
    131, 137, 139, 149, 151, 157};

	const uint8_t block2[] = {163, 167, 173, 179, 181, 191, 193, 197, 199, 211,
    223, 227, 229, 233, 239, 241, 251};

	uint32_t computedCRC1 = crc32c(0, block1, sizeof(block1));
	uint32_t computedCRC2 = crc32c(~computedCRC1, block2, sizeof(block2));
	uint32_t computedCRC3 = crc32c(~crc32c(0, block1, sizeof(block1)), block2, sizeof(block2));

	assert(computedCRC1 == 0xE8174F48);
	assert(computedCRC2 == 0x56DAB0A6);
	assert(computedCRC3 == 0x56DAB0A6);

	computedCRC1 = GenerateRSCRC(0, sizeof(block1), block1);
	computedCRC2 = GenerateRSCRC(computedCRC1, sizeof(block2), block2);
	computedCRC3 = GenerateRSCRC(GenerateRSCRC(0, sizeof(block1), block1), sizeof(block2), block2);

	assert(computedCRC1 == 0x733D4DCA);
	assert(computedCRC2 == 0x754ED37E);
	assert(computedCRC3 == 0x754ED37E);

	return 0;
}

