#include "fnv_hash.h"

// implementation details
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
uint64_t fnv_hash(void* data, size_t len) {
	// algorithm parameters
	static const uint64_t FNV_offset_basis = 0xcbf29ce484222325;
	static const uint64_t FNV_prime = 0x00000100000001b3;

	uint64_t hash = FNV_offset_basis;
	for (size_t i = 0; i < len; i++) {
		const unsigned char byte = ((char*)data)[i];
		hash *= FNV_prime;
		hash ^= byte;
	}

	return hash;
}
