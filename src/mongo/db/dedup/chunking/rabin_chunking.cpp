/**
 * @file rabin_chunking.cpp
 * @brief Contains functions used to call rabin fingerprinting on input data
 * and divide them into chunks.
 *
 * @author Sudhir Vijay
 * @version 1.0
 * @date 2016-01-31
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include <cstdlib>
#include <string>
#include <vector>

#include "dedup.h"
#include "mongo/util/log.h"
#include "rabin_chunking.h"

#define MIN_SEG_SIZE 2048
#define MAX_SEG_SIZE 8192
#define DEFAULT_RABIN_WINDOW_SIZE 128

namespace mongo {
    using std::endl;
    namespace dedup {
        /* RabinChunking constructor */
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

        /**
         * Function that initializes a new rabin polynomial, finds chunk
         * boundaries and fills them in the chunkOffset and chunkLength vectors
         * */
        void RabinChunking::rabinChunk( unsigned char *bytes, int64_t size,
                std::vector<int64_t> &chunkOffset, std::vector<int64_t>
                &chunkLength)
        {
            int rabin_window_size = DEFAULT_RABIN_WINDOW_SIZE;
            /** Initializing rabin polynomial */
            rabinpoly_t *rp = rabin_init(rabin_window_size, avgChunkSize,
                    minChunkSize, maxChunkSize);

            if (rp == NULL) {
                return;
            }

            char *buftoread = reinterpret_cast<char*> (bytes);
            int64_t segment_len = 0, cur_segment_len = 0, segment_offset = 0,
                    size_to_read = size;
            int new_segment = 0;

            /**
             * Run rabin segmentation over the input buffer. Fill chunkLength
             * and chunkOffset vectors with found rabin boundary coordinates.
             * */
            while ((cur_segment_len = rabin_segment_next(rp, buftoread, size,
                            &new_segment)) > 0) {
                size_to_read -= cur_segment_len;
                if (size_to_read < 0) {
                    break;
                }

                segment_len += cur_segment_len;

                if (new_segment) {
                    segment_offset += segment_len;
                    chunkLength.push_back(segment_len);
                    chunkOffset.push_back(segment_offset);
                    segment_len = 0;
                    new_segment = 0;
                }

                if (size_to_read == 0) {
                    break;
                }

                buftoread += cur_segment_len;
            }

            if (cur_segment_len == -1) {
                rabin_free(&rp);
                return;
            }

            if (segment_len > 0) {
                segment_offset += segment_len;
                chunkLength.push_back(segment_len);
                chunkOffset.push_back(segment_offset);
            }

            rabin_free(&rp);
        }
    }
}
