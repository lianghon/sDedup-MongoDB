#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include <stdio.h>
#include "chunk_index.h"
#include "mongo/util/timer.h"
#include "mongo/util/log.h"
#include <boost/thread/thread.hpp>

namespace mongo {
    namespace dedup {

        ChunkIndex::ChunkIndex(std::string fName, uint64_t numDocs) :
            flashFile (fName, PAGE_SIZE, true),
            rebuildDone (false),
            readPage (PAGE_SIZE),
            writePage (PAGE_SIZE),
            ramBuffer (NUM_PAGE_ENTRIES),
            prefetchCache (LRUCount * NUM_PAGE_ENTRIES),
            sha1HT (numDocs, SHA1, flashFile, readPage, 
                    ramBuffer, prefetchCache, lruMap, lruList),
            featureHT (numDocs * NUM_FEATURES, FEATURE, flashFile, readPage, 
                    ramBuffer, prefetchCache, lruMap, lruList) {	
            }


        void ChunkIndex::rebuild()
        {
            int i, j, k, sha1Inserted = 0, ftInserted = 0;
            long millis = 0;
            Timer *timer = new Timer();
            std::vector<unsigned char> docMeta = flashFile.readAll();
            int numPages = docMeta.size() / PAGE_SIZE;
            PageData page(PAGE_SIZE);
            for (i = 0; i < numPages; ++i) {
                std::vector<unsigned char> meta(docMeta.begin() + i * PAGE_SIZE, 
                        docMeta.begin() + (i + 1) * PAGE_SIZE);
                // parse one page of document metadata 
                page.convertFromBytes(meta);
                int pageId = page.getPageId();
                DEDUP_LOG() << "rebuild page: " << pageId << ", slots: " << page.itemsCount(); 

                // Insert into sha1 hash table and feature hash table
                for (j = 0; j < page.itemsCount(); ++j) {
                    sha1HT.insert(page.keys[j].sha1, pageId, j);
                    sha1Inserted += 1;

                    for (k = 0; k < NUM_FEATURES; ++k) {
                        unsigned char *ft = page.keys[j].features[k];
                        if (!emptyFeature(ft)) {
                            featureHT.insert(ft, pageId, j);
                            ftInserted += 1;
                        }
                    }
                }

            }
            millis += timer->millis(); 
            DEDUP_LOG() << "Rebuild done: read " << numPages << " pages. sha1Inserted: " << sha1Inserted << ", ftInserted: " << ftInserted << ". Time: " << millis << " milliseconds";
            delete timer;
        }

        void ChunkIndex::addToFlashAndNewContainer()
        {
            ramBuffer.clear();
            // write page data to flash
            writePage.setPageId(flashFile.getCurrentPageId());

            // insert page data into cookoo hash table
            // TODO: const reference to the object
            // hashTable.insertPage(writePage);

            flashFile.fileWritePage(writePage.convertToBytes());

            // pageId is reset to 0 here.
            writePage.resetPageMetaData();
            // when flash space is used up, start over from the beginning
            // use flash as a log
            if (flashFile.getCurrentPageId() == maxFlashSizeInPages) {
                flashFile.setCurrentPageId(0);
            }
        }


        void ChunkIndex::set(ChunkHash cHash, DiskLoc &dLoc, int numFeatures) 
        {
            if (writePage.isPageFull()) {
                addToFlashAndNewContainer();				
            }

            // use pageId from flashFile instead of writePage.
            int pageId = flashFile.getCurrentPageId();
            // add to page data
            int loc = writePage.addToPage(cHash, dLoc);

            //PageLoc pLoc(pageId, loc);
            //hashTable.insertSha1(cHash.sha1, pLoc);
            sha1HT.insert(cHash.sha1, pageId, loc);

            for (int j = 0; j < numFeatures; ++j) {
                featureHT.insert(cHash.features[j], pageId, loc);
            }

            // add to ram buffer
            Bucket b(pageId, loc);
            ramBuffer[ LOC(b) ] = MetaData(cHash, dLoc);

            DEDUP_DEBUG() << "LX: set: pageId: " << pageId << ", loc: " << loc
                << ". ns: " << std::string(dLoc.ns) << ", oid: " << dLoc.oid.toString();
        }

        void ChunkIndex::remove(const ChunkHash &cHash, int numFeatures)
        {
            sha1HT.remove(cHash.sha1, cHash);
            for (int i = 0; i < numFeatures; ++i)
                featureHT.remove(cHash.features[i], cHash);
        }


        int ChunkIndex::index(
                const ChunkHash & cHash,
                int numFeatures,
                DiskLoc &dLoc,
                MetaData &simDoc,
                bool setIndex,
                const objMap &cache)
        {
            int ret;	
            std::string sha1Key = byte2String(cHash.sha1, SHA1_LENGTH); 
            //DEDUP_LOG() << "index: Sha1: " << sha1Key;

            /*
               for (int k = 0; k < numFeatures; ++k) {
               std::string ftStr = byte2String(cHash.features[k], FEATURE_LENGTH);
               DEDUP_DEBUG() << "index: Feature " << k << " :" << ftStr;
               }

               MetaData srcPE = getBySha1(cHash);
               */
            std::vector<MetaData> srcMd = sha1HT.find(cHash.sha1, false);
            if ( srcMd.size() > 0 ) {	
                simDoc = srcMd[0];
                std::string matchSha1 = byte2String(simDoc.ch.sha1, SHA1_LENGTH);
                //DEDUP_LOG() << "Whole duplicate, match Sha1: " << sha1Key;
                ret = 0;	// whole blob duplicate
            } else {
                // look for similar blobs
                unsigned int i;
                std::vector<MetaData> simDocs, candidates;
                for (i = 0; i < (unsigned int)numFeatures; ++i) {
                    candidates = featureHT.find(cHash.features[i]);
                    simDocs.insert(simDocs.end(), candidates.begin(), candidates.end());
                }

                if (simDocs.size() > 0) {
                    ret = 1;	// found similar blob(s)
                    std::vector<MetaData> matches;
                    std::vector<int> counts;

                    // get a map from doc to #matches in matches
                    for (std::vector<MetaData>::iterator it = simDocs.begin();
                            it != simDocs.end(); ++it) {
                        for (i = 0; i < matches.size(); ++i) {
                            if (matches[i] == *it) {
                                counts[i]++;
                                break;
                            }
                        }
                        if (i == matches.size()) {
                            matches.push_back(*it);
                            counts.push_back(1);
                        }
                    }

                    // find the most similar blob (with max #occurences and appeared later)
                    int maxI = 0;
                    int maxC = 0;
                    for (i = 0; i < counts.size(); ++i) {
                        if ( counts[i] > maxC ) {
                            maxC = counts[i];
                            maxI = i;
                        }
                    }

                    int maxCount = 0;
                    int maxIndex = 0;
                    for (i = 0; i < counts.size(); ++i) {
                        DEDUP_DEBUG() << "LX: index: match oid: " 
                            << matches[i].dl.oid.toString()  
                            << ". count: " << counts[i];
                        objMap::const_iterator it = cache.find(matches[i].dl.oid.toString()); 
                        if (it != cache.end()) {
                            // in-cache entries get higher rank scores
                            counts[i] += 4;
                            DEDUP_DEBUG() << "LX: index: match in cache: " << matches[i].dl.oid.toString(); 
                        }

                        if ( counts[i] > maxCount ) {
                            maxCount = counts[i];
                            maxIndex = i;
                        }
                    }

                    DEDUP_DEBUG() << "LX: maxCount: " << maxCount << ". maxC: " << maxC;
                    if (maxIndex != maxI) 
                        DEDUP_DEBUG() << "LX: choose sub-optimal match with cache hit!";

                    DEDUP_DEBUG() << "LX: index: match doc : score: " << maxCount << ", ns: " 
                        << std::string(matches[maxIndex].dl.ns)
                        //<< ", sha1: " << byte2String(matches[maxIndex].ch.sha1, SHA1_LENGTH);
                        << ". OID: " << matches[maxIndex].dl.oid.toString();

                    //simDoc = matches[maxI];
                    simDoc = matches[maxIndex];
                } else {
                    ret = 2;
                }

                /*
                // TEST - don't insert indexes for similar docs
                if (ret == 1) {
                setIndex = false;
                }
                // add sha1 and features to dedup index
                if(setIndex) {
                set(cHash, dLoc, numFeatures);
                }
                */
            }

            return ret;
        }

        void ChunkIndex::printStats()
        {
        }

        /*
           MetaData ChunkIndex::getBySha1(const ChunkHash &cHash)
           {
           MetaData md;
           std::string sha1Key = byte2String(cHash.sha1, SHA1_LENGTH); 
           DEDUP_DEBUG() << "LX: getBySha1: " << sha1Key;

           PageLoc pLoc = hashTable.findSha1(cHash.sha1);

           if ( !VALID_PID(pLoc.pageId) ) {
        // return empty diskLoc, if not found
        return md;
        } else {
        // try to find mapping from pageLoc to diskLoc
        // first try ram buffer
        fdMap::iterator it = ramBuffer.find( LOC(pLoc) );
        if (it != ramBuffer.end()) {
        md = (*it).second;
        DEDUP_DEBUG() << "LX: getBySha1: found in ram buffer";
        } 
        else {	
        // try prefetch cache
        it = prefetchCache.find( LOC(pLoc) );
        if (it != prefetchCache.end()) {
        md = (*it).second;
        DEDUP_DEBUG() << "getBySha1: found in prefetch cache"; 
        } 
        else {
        // read from flash and add to prefetch cache
        DEDUP_DEBUG() << "getBySha1: read from flash and add to prefetch cache.";
        addToPrefetchCache(pLoc.pageId);
        md = prefetchCache[ LOC(pLoc) ];
        }
        }
        }
        return md;
        }

        std::list<MetaData> ChunkIndex::getByFeature(
        const ChunkHash &cHash, int featureID)
        {	
        std::string ftKey = byte2String(cHash.features[featureID], FEATURE_LENGTH); 
        DEDUP_DEBUG() << "getByFeature: " << ftKey;
        std::list<MetaData> mdList;

        boost::array<PageLoc, maxBuckets> plArray = 
        hashTable.findFeature(cHash.features[featureID]);

        for (unsigned int i = 0; i < maxBuckets; ++i) {
        if (!VALID_PID(plArray[i].pageId)) 
        break;

        fdMap::iterator it = ramBuffer.find( LOC(plArray[i]) );
        if (it != ramBuffer.end() ) {
        DEDUP_DEBUG() << "getByFeature: found in rambuffer";
        mdList.push_back((*it).second);
        } else {
        it = prefetchCache.find( LOC(plArray[i]) );
        if (it != prefetchCache.end()) {
        DEDUP_DEBUG() << "getByFeature: found in prefetch cache.";
        mdList.push_back((*it).second);
        } else {
        // read from flash and add to prefetch cache
        DEDUP_DEBUG() << "getByFeature: read from flash and add to prefetch cache.";
        addToPrefetchCache(plArray[i].pageId);
        mdList.push_back(prefetchCache[ LOC(plArray[i]) ]);
        }
        }
        }
        return mdList;
        }
        */


    }
}
