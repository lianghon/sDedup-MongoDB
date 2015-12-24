// rabin_chunking.h
#pragma once
#include <stdint.h>
#include <vector>

namespace mongo{
	namespace dedup {

		class RabinChunking
		{
		private:
			int64_t minChunkSize;
			int64_t maxChunkSize;
			int64_t avgChunkSize;
			int64_t bufferSize;
			
		public:
			RabinChunking(
				int64_t minChunkSize,
				int64_t maxChunkSize,
				int64_t avgChunkSize,
				int64_t bufferSize);

			void rabinChunk(
				unsigned char *bytes,
				int64_t size,
				std::vector<int64_t> &chunkOffset,
				std::vector<int64_t> &chunkLength);
		};

	}
}
