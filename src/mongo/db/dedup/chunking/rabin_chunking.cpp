#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "rabin_chunking.h"
#include <vector>
#include <cstdlib>
#include <string>
#include "dedup.h"
#include "mongo/util/log.h"

#define MIN_SEG_SIZE 2048
#define MAX_SEG_SIZE 8192

namespace mongo {
    using std::endl;
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
                log() << "*~ Rabin initialization ~*" << endl;
                log() << "Min size " << minSize << endl;
                log() << "Max size " << maxSize << endl;
                log() << "Avg size " << avgSize << endl;
                log() << "Buf size " << bufSize << endl;
                log() << "************ " << endl;
            }

        void RabinChunking::rabinChunk( unsigned char *bytes, int64_t size,
                std::vector<int64_t> &chunkOffset, std::vector<int64_t>
                &chunkLength)
        {
            log() << "Rabin called with Size -  " << size << endl;
            //int avg_seg_size = 4096;
            int rabin_window_size = 128;
            rabinpoly_t *rp = rabin_init(rabin_window_size, avgChunkSize,
                    minChunkSize, maxChunkSize);

            if (rp == NULL) {
                log() << "Rabin initialization failed" << endl;
                return;
            }

            char *buftoread = reinterpret_cast<char*> (bytes);
            int64_t segment_len = 0, cur_segment_len = 0, segment_offset = 0,
                    size_to_read = size;
            int new_segment = 0;

            chunkOffset.push_back(segment_offset);

            while ((cur_segment_len = rabin_segment_next(rp, buftoread, size,
                            &new_segment)) > 0) {
                size_to_read -= cur_segment_len;
                log() << "size_to_read:" << size_to_read << endl;
                if (size_to_read < 0) {
                    log() << "Breaking [size_to_read < 0]!" << endl;
                    break;
                }

                segment_len += cur_segment_len;

                if (new_segment) {
                    log() << "New segment found. Segment Length:"<< segment_len << endl;
                    segment_offset += segment_len;
                    chunkLength.push_back(segment_len);
                    chunkOffset.push_back(segment_offset);
                    segment_len = 0;
                    new_segment = 0;
                }

                if (size_to_read == 0) {
                    log() << "Breaking [size_to_read == 0]!" << endl;
                    break;
                }

                buftoread += cur_segment_len;
            }

            if (cur_segment_len == -1) {
                log() << "Invalid len!" << endl;
                rabin_free(&rp);
                return;
            }

            if (segment_len > 0) {
                chunkLength.push_back(segment_len);
            }

            rabin_free(&rp);
            log() << "Returning from rabin" << endl;
        }
    }
}
