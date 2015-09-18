#include "rabin_chunking.h"
//#include <iostream>
//#include <algorithm>

namespace mongo {
	namespace dedup {
		
		RabinChunking::RabinChunking(
			int64_t minSize,
			int64_t maxSize,
			int64_t avgSize,
			int64_t bufSize) :
			minChunkSize(minSize),
			maxChunkSize(maxSize),
			avgChunkSize(avgSize),
			bufferSize(bufSize) {
		}


		void RabinChunking::rabinChunk(
			unsigned char *bytes,
			int64_t size,
			std::vector<int64_t> &chunkOffset,
			std::vector<int64_t> &chunkLength)
		{
		}

	}
}
