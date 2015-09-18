#include "mongo/db/dedup/dedup_setup.h"
#include <memory>

namespace mongo {

    bool dedup::verboseDedupLogging = true;
    bool dedup::verboseDedupDebugging = false;

    // initialize dedup index
    int avgChunkSize = 256;
    int64_t numDocs = 500000;
    int64_t chunkBufferSize = 1024 * 64;
    std::string flashFile("/tmp/flash02");
    int cacheSize = 2000;

    std::auto_ptr<dedup::PDedup> pdedup(
        new dedup::PDedup(numDocs, avgChunkSize, chunkBufferSize, flashFile, cacheSize));
}
