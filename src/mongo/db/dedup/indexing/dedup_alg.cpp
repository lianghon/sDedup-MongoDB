#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/db/dedup/chunking/sha1.h"
#include "mongo/db/dedup/indexing/dedup_alg.h"
#include "mongo/util/log.h"
#include <algorithm>
#include <iostream>
using namespace std;
#include <boost/thread/thread.hpp> 
#include <cstdlib>
#include <vector>
#include <string>
//#include "mongo/db/dedup/chunking/rabin64_hash.h"

using namespace mongo;

namespace mongo{
    namespace dedup{

        DedupAlg::DedupAlg() :
            totalBytes (0),
            dupBytes (0),
            storedBytes (0),
            totalBlobs (0),
            dupBlobs (0),
            uniqueBlobs (0),
            uniqueBytes (0),
            totalChunks (0),
            elapsedMicros (0) {
            }


        void DedupAlg::printStats()
        {
            if(totalBlobs % 100 == 0) {
                DEDUP_LOG() << std::endl;
                DEDUP_LOG() << "Total blob count: " << totalBlobs << ". Size: " << (totalBytes >> 10) << "KB";
                DEDUP_LOG() << "Duplicate blob count: " << dupBlobs << ". Size: " << (dupBytes >> 10) << "KB";
                DEDUP_LOG() << "Unique blob count: " << uniqueBlobs << ". Size: " << (uniqueBytes >> 10) << "KB";
                DEDUP_LOG() << "Stored size: " << (storedBytes >> 10) << "KB";
                DEDUP_LOG() << "Dedup ratio: " << (double)storedBytes / totalBytes * 100 << "%";
                DEDUP_LOG() << "Throughput: " << totalBytes / elapsedMicros * 1000000 / 1024 / 1024 << " MB/s";
            }
        }

        void PDedup::profile()
        {
            if(totalBlobs % 100 == 0) {
                DEDUP_LOG() << "Total time: " << elapsedMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Chunk time: " << chunkMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Sample time: " << sampleMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Index time: " << indexMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Delta time: " << deltaMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Delta source fetch time: " << deltaFetchMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Cache fetch time: " << cacheFetchMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "DB fetch time: " << dbFetchMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Delta computation time: " << deltaComputeMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Delta index time: " << deltaIndexMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Delta match time: " << deltaMatchMicros / 1000000 << " seconds.";
            }
        }

        void PDedup::profileSecondary()
        {
            if(totalBlobs % 100 == 0) {
                DEDUP_LOG() << "Total blob count: " << totalBlobs << ". Size: " << (totalBytes >> 10) << "KB";
                DEDUP_LOG() << "Total time: " << elapsedMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Decompression throughput: " << totalBytes / elapsedMicros * 1000000 / 1024 / 1024 << " MB/s";
            }
        }

        PDedup::PDedup(int64_t numDocs, int64_t avgChkSize, int64_t chunkBufSize, 
                std::string flashFileName, int cSize) :
            avgChunkSize (avgChkSize),
            rChunk (avgChkSize >> 4, avgChkSize << 4,
                    avgChkSize, chunkBufSize),
            cIndex (flashFileName, numDocs), 
            cacheSize (cSize),
            sampledChunks (0),
            diskAccesses (0),
            chunkMicros (0),
            indexMicros (0),
            sampleMicros (0),
            deltaMicros (0),
            deltaFetchMicros (0),
            deltaComputeMicros (0),
            deltaIndexMicros (0),
            deltaMatchMicros (0),
            cacheFetchMicros (0),
            dbFetchMicros (0),
            cacheLookups (0),
            cacheHits (0),
            numMilestones (0),
            numIndexes (0) {
            }

        PDedup::~PDedup()
        {
        }

        void PDedup::printStats()
        {
            if(totalBlobs % 100 == 0) {
                DEDUP_LOG() << "Total chunk count: " << totalChunks
                    << ". Sampled chunk count: " << sampledChunks
                    << ". Sampling rate: " << (double)sampledChunks / totalChunks;
                DEDUP_LOG() << "Obj cache size: " << objCache.size();
                if (cacheLookups > 0) {
                    DEDUP_LOG() << "Cache lookups: " << cacheLookups << ". Cache hits: "
                        << cacheHits << ". Hit ratio: " << (1.0 * cacheHits / cacheLookups);
                }
                DEDUP_LOG() << "Number of indexes: " << numIndexes;
                DEDUP_LOG() << "Number of milestones: " << numMilestones;
            }
            DedupAlg::printStats();
            cIndex.printStats();
        }

        std::string PDedup::algName()
        {
            return std::string("PDedup");
        }

        /**
         * TODO: Need to fill in fullRabinHash and incRabinHash with hashing
         * derived from rabin-fingerprinting. Used for compression.
         * */
        //void PDedup::fullRabinHash(const unsigned char *bytes, int offset, uint64_t &hash)
        //{
            //uint64_t origHash;
            //// reset hash value
            //hash = 0;
            //for (int i = 0; i < WINDOW_SIZE; ++i) {
                //origHash = hash;
                //hash <<= 8;
                //hash ^= bytes[offset + i];
                //hash ^= g_TD[(origHash >> 56) & 0xff];
            //}
        //}

        //void PDedup::incRabinHash(const unsigned char *bytes, int offset, uint64_t &hash)
        //{
            //uint64_t origHash;
            //if (offset < 1)
                //return;
            //hash ^= g_TU[bytes[offset - 1]];
            //origHash = hash;
            //hash <<= 8;
            //hash ^= bytes[offset - 1 + WINDOW_SIZE];
            //hash ^= g_TD[(origHash >> 56) & 0xff];
        //}

        void PDedup::addToLRU(const std::string& objId)
        {
            OIDLru.push_back(objId);
            if (objCache.size() > cacheSize) {
                // remove 1 or more objects from the cache
                // OIDLru may contain elements that were replaced in the objCache
                objMap::iterator it;
                do {
                    objCache.erase(OIDLru.front());
                    OIDLru.pop_front();
                } while ( (it = objCache.find(OIDLru.front())) == objCache.end() );
            }
        }

        int PDedup::getObjFromOid(std::string hostAndPort, 
                const mongo::OID &srcOID, std::string ns, BSONObj &obj)
        {
            Timer *timer = new Timer();

            DEDUP_DEBUG() << "LX: getObjFromOid: host:" << hostAndPort 
                << ", srcOID: " << srcOID.toString() << ", ns: " << ns;
            // First try source object cache

            cacheLookups++;
            objMap::iterator it = objCache.find(srcOID.toString());
            if ( it != objCache.end() ) {
                // Found in cache, no need to get its own copy
                obj = (*it).second;
                cacheHits++;
                cacheFetchMicros += timer->micros();
                DEDUP_DEBUG() << "LX: getObjFromOid: " << srcOID.toString() << "  found: 1";
                return 0;
            }
            DEDUP_DEBUG() << "LX: getObjFromOid: " << srcOID.toString() << "  found: 0";
            // Not found in obj cache, try query from host
            // setup connection to localhost
            try {
                int ret;
                timer->reset();
                DBClientConnection conn;
                conn.connect(hostAndPort);
                if (conn.isFailed())
                    error() << "LX: connection to " << hostAndPort << " failed!";

                DEDUP_DEBUG() << "LX: getObjFromOid: query host: " << hostAndPort;
                int queryOptions = QueryOption_SlaveOk;
                std::auto_ptr<DBClientCursor> cursor
                    = conn.query(ns, QUERY("_id" << srcOID), 0, 0, 0, queryOptions);

                if(!cursor->more()) {
                    DEDUP_DEBUG() << "LX: getObjFromOid: no results found!";
                    ret = -1;
                } else {
                    DEDUP_DEBUG() << "LX: getObjFromOid: found in database!";
                    obj = cursor->next().getOwned();
                    // TEST - for local dedup, put src in the cache
                    // Normal case: put dst in the cache
                    // objCache[srcOID.toString()] = obj;
                    ret = 0;
                }
                double thisFetchMicros = timer->micros();
                dbFetchMicros += thisFetchMicros;
                DEDUP_DEBUG() << "ns: " << ns << ". objId: " << srcOID.toString() << ". db fetch micros: " << thisFetchMicros;
                return ret;
            } catch( const mongo::DBException &e ) {
                error() << "LX: getObjFromOid: DBException reading source blob. " << e.what();
                return -1;
            } catch (...) {
                error() << "LX: getObjFromOid: Unknown Exception.";
                return -1;
            }
        }

        int PDedup::deltaCompress(
                const unsigned char *src, int srcLen, 
                const unsigned char *dst, int dstLen,
                std::vector<Segment> &matchSeg, 
                std::vector<unsigned char> &unqBytes)
        {
            DEDUP_DEBUG() << "LX: deltacompress: srcLen: " << srcLen << " dstLen: " << dstLen;
            int i = 0;
            int j;
            int matchLen = 0;
            uint64_t hash = 0;
            int uniqueSegBegin = 0;
            bool getFullHash = true;

            Timer *timer = new Timer();

            // build index for src, use custom allocator
            // should be faster than default allocator			
            /*
               boost::unordered_map<uint64_t, int,
               boost::hash<uint64_t>,
               std::equal_to<uint64_t>,
               boost::fast_pool_allocator<std::pair<const uint64_t, int>>> sIndex;
               */

            // Make sure these vectors are empty 
            matchSeg.clear();
            unqBytes.clear();

            boost::unordered_map<uint64_t, int> sIndex;

            if (DELTA_SAMPLE_INTVL <= WINDOW_SIZE) {
                while (i <= srcLen - WINDOW_SIZE)
                {
                    if (getFullHash) {
                        //fullRabinHash(src, i, hash);
                        getFullHash = false;
                    } else {
                        //incRabinHash(src, i, hash);
                    }

                    // sample src index
                    // amortize the cost of map lookup and insertion
                    // TODO: evaluate loss of dedup quality

                    if (i % DELTA_SAMPLE_INTVL == 0 && 
                            sIndex.find(hash) == sIndex.end()) {
                        sIndex[hash] = i;
                    }
                    i++;
                }
            } else {
                while (i <= srcLen - WINDOW_SIZE)
                {
                    //fullRabinHash(src, i, hash);
                    // sample src index
                    // amortize the cost of map lookup and insertion
                    // TODO: evaluate loss of dedup quality
                    if (i % DELTA_SAMPLE_INTVL == 0 && 
                            sIndex.find(hash) == sIndex.end()) {
                        sIndex[hash] = i;
                    }
                    i += DELTA_SAMPLE_INTVL;
                }
            }
            deltaIndexMicros += timer->micros();
            timer->reset();
            /*
               DEDUP_LOG() << "Time to build src index: " << secs;
               DEDUP_LOG() << "map seconds: " << mapSecs;
               DEDUP_LOG() << "hash seconds: " << hashSecs;
               DEDUP_LOG() << "num of entries inserted: " << entries;
               */

            // build index for dst and check for match in src index
            i = 0; hash = 0; getFullHash = true;

            while (i <= dstLen - WINDOW_SIZE)
            {
                if (getFullHash) {
                    //fullRabinHash(dst, i, hash);
                    getFullHash = false;
                } else {
                    //incRabinHash(dst, i, hash);
                }

                if (sIndex.find(hash) != sIndex.end()) {
                    // end of previous unique segment
                    if (i > uniqueSegBegin) {
                        int segOff = unqBytes.size();
                        int segLen = i - uniqueSegBegin;
                        Segment uniqueSeg(UNQ_SEG, segOff, segLen);
                        matchSeg.push_back(uniqueSeg);


                        // Fill unqBytes with dst 
                        for (int k = 0; k < segLen; ++k) {
                            unqBytes.push_back(dst[uniqueSegBegin + k]);
                        }

                        uniqueSegBegin = -1;
                    }

                    // find a match in src, extend search for longest match
                    int srcOff = sIndex[hash];
                    j = WINDOW_SIZE;
                    while ( srcOff + j < srcLen	&&
                            i + j < dstLen	&&
                            src[srcOff + j] == dst[i + j])
                        j++;

                    i += j;
                    matchLen += j;
                    getFullHash = true;
                    uniqueSegBegin = i;	// begin of a new unique segment
                    Segment dupSeg(DUP_SEG, srcOff, j);
                    matchSeg.push_back(dupSeg);
                } else { 
                    i++; 
                }

            }

            if (uniqueSegBegin != -1 && uniqueSegBegin < dstLen) {
                // push the last unique segment into vector
                int segOff = unqBytes.size();
                int segLen = dstLen - uniqueSegBegin;
                Segment uniqueSeg(UNQ_SEG, segOff, segLen);
                matchSeg.push_back(uniqueSeg);

                // Fill unqBytes with dst 
                for (int k = 0; k < segLen; ++k) {
                    unqBytes.push_back(dst[uniqueSegBegin + k]);
                }

            }

            deltaMatchMicros+= timer->micros();
            delete timer;
            return matchLen;
        }


        // Should not call this function together with processBlob
        // in a single mongod instance.
        void PDedup::deltaDeCompress(const BSONObj &srcObj, 
                BSONObj &dstObj,
                char *unqBytes,
                const std::vector<Segment> &matchSeg)
        {
            BSONObjBuilder bbld;
            bbld.append(srcObj["_id"]);
            const char * src = srcObj.objdata() + 4 + srcObj["_id"].size();
            DEDUP_DEBUG() << "LX: PDedup::deltaDeCompress start.";
            int pos = 0;
            Timer *timer = new Timer();

            for (std::vector<Segment>::const_iterator it = matchSeg.begin();
                    it != matchSeg.end(); ++it) {			
                if ((*it).type == DUP_SEG) {
                    DEDUP_DEBUG() << "DUP: " << pos << "\t" << (*it).offset 
                        << "\t" << (*it).len;
                    bbld.bb().appendBuf(src + (*it).offset, (*it).len);
                }
                else if ((*it).type == UNQ_SEG) {
                    DEDUP_DEBUG() <<"UNQ: " << pos << "\t" << (*it).offset 
                        << "\t" << (*it).len;
                    bbld.bb().appendBuf(unqBytes + (*it).offset, (*it).len);
                }
            }
            dstObj = bbld.obj();
            totalBlobs++;
            totalBytes += dstObj.objsize();
            elapsedMicros += timer->micros();
            // update obj cache
            std::string srcObjId = srcObj["_id"].OID().toString();
            std::string dstObjId = dstObj["_id"].OID().toString();
            objCache.erase(srcObjId);
            objCache[dstObjId] = dstObj.getOwned();
            addToLRU(dstObjId);

            delete timer;
        }

        int PDedup::processBlob(
                const BSONObj &obj,
                DiskLoc &dLoc, 
                DiskLoc &sLoc,
                std::vector<Segment> &matchSeg,
                std::vector<unsigned char> &uniqueData,
                bool setIndex)
        {
            boost::mutex::scoped_lock lk(_m);

            /*
               if(!cIndex.rebuildDone) {
               cIndex.rebuildDone = true;
               boost::thread rebuildThread(&ChunkIndex::rebuild, &cIndex);
            //cIndex.rebuild();
            }
            */

            int i;
            BSONElement oidE = obj["_id"];
            int len = obj.objsize() - 5 - oidE.size();
            const char * bytes = obj.objdata() + 4 + oidE.size();

            std::vector<int64_t> chunkOffset, chunkLen;
            Timer *timer = new Timer();

            rChunk.rabinChunk((unsigned char *)bytes, len, chunkOffset, chunkLen);
            //assert(chunkOffset.size() == chunkLen.size());

            DEDUP_DEBUG() << "LX: done chunking."; 
            chunkMicros += timer->micros();
            timer->reset();

            // sha1 hash of the entire blob
            unsigned char sha1key[SHA1_LENGTH];
            sha1((unsigned char *)bytes, len, sha1key);

            // sorted Murmur hashes for all chunks. The top NUM_FEATURES 
            // hashes are called features ( or 1 sketch) of a blob and added 
            // to index. #chunks may be less than NUM_FEATURES, 
            // in which case, the sketch only consists of #chunks features.
            std::vector<uint64_t> features;
            uint64_t feature;

            for (i = 0; i < chunkOffset.size(); ++i) {
                feature = MurmurHash64A(bytes + chunkOffset[i], chunkLen[i], 0);			
                features.push_back(feature);

                std::string ftStr = byte2String((unsigned char *)&feature, FEATURE_LENGTH);
                //DEDUP_DEBUG() << "Feature string: " << ftStr << " Chunk length: " << chunkLen[i];
            }

            // sort features in a consistent way
            std::sort(features.begin(), features.end());

            features.erase(std::unique(features.begin(), features.end()), features.end());

            ChunkHash cHash;
            memcpy(cHash.sha1, sha1key, SHA1_LENGTH);
            int numFeatures = features.size() < NUM_FEATURES ?
                features.size() : NUM_FEATURES;

            /*
            // random sampling of features
            for (int j = 0; j < numFeatures; ++j) {
            i = rand() % features.size();
            memcpy(cHash.features[j],
            (unsigned char *)(&features[i]), FEATURE_LENGTH);
            }
            */

            for (i = 0; i < numFeatures; ++i) {
                memcpy(cHash.features[i], 
                        (unsigned char *)(&features[i]), FEATURE_LENGTH);
            }

            sampleMicros += timer->micros();
            timer->reset();

            // add sha1 hash and sketch to index
            MetaData md;
            int indexCode = cIndex.index(cHash, numFeatures, dLoc, md, setIndex, objCache);
            sLoc = md.dl;
            DEDUP_DEBUG() << "LX: processBlob: Index code: " << indexCode << ". OID: " << oidE.toString();
            DEDUP_DEBUG() << "LX: processBlob: src loc ns: " << sLoc.ns;

            indexMicros += timer->micros();
            timer->reset();

            // interpret index results
            if (indexCode == 0) {
                dupBlobs++;
                dupBytes += len;
            }
            else if (indexCode == 1) {
                //DEDUP_DEBUG() << "LX: processBlob: src oid: " << sLoc.oid.toString() 
                //    << ". dst oid: " << dLoc.oid.toString();

                BSONObj srcObj;
                if ( getObjFromOid("localhost:27017", 
                            sLoc.oid, sLoc.ns, srcObj) == -1 ) {
                    // The source document has been deleted from the client
                    // database, need to delete its entries from the dedup index 
                    ChunkHash cHash = md.ch;
                    cIndex.remove(cHash, numFeatures);
                    DEDUP_LOG() << "LX: processBlob: cannot find document in local database: " << sLoc.oid.toString();
                    // treate as unique doc
                    indexCode = 2;
                    goto unique;
                }

                BSONElement srcOidE = srcObj["_id"];    
                int srcLen = srcObj.objsize() - 5 - srcOidE.size();
                const char * src = srcObj.objdata() + 4 + srcOidE.size();

                deltaFetchMicros += timer->micros();
                timer->reset();

                // only store unique bytes after delta compression
                // match segments are populated but not used for now				
                int numUnqBytes = len - deltaCompress(
                        (unsigned char *)src, srcLen, (unsigned char *)bytes, len, matchSeg, uniqueData);
                storedBytes += numUnqBytes + matchSeg.size() * sizeof(Segment);

                int deltaLen = numUnqBytes + matchSeg.size() * sizeof(Segment);
                std::string dstObjId = oidE.OID().toString();
                std::string srcObjId = srcOidE.OID().toString();
                //DEDUP_LOG() << "srcObj: " << srcObjId << ", dstObj: " << dstObjId;  
                //DEDUP_LOG() << "srcLen: " << srcLen << ", dstLen: " << len << ", delta len: " << deltaLen; 
                // Milestone doc
                if(deltaLen >= 0 * len) {
                    cIndex.set(cHash, dLoc, numFeatures);
                    numMilestones++;
                    numIndexes += numFeatures;
                    DEDUP_DEBUG() << "Insert index, numFeatures:" << numFeatures << ", total indexes: " << numIndexes;
                    // replace cache
                    std::string dstObjId = oidE.OID().toString();
                    std::string srcObjId = srcOidE.OID().toString();
                    objCache.erase(srcObjId);
                    objCache[dstObjId] = obj.getOwned();
                    addToLRU(dstObjId);
                }

                // TODO: replace cache entries
                /*
                   std::string dstObjId = oidE.OID().toString();
                   std::string srcObjId = srcOidE.OID().toString();
                   objCache.erase(srcObjId);
                   objCache[dstObjId] = obj.getOwned();
                   addToLRU(dstObjId);
                   */

                //DEDUP_DEBUG() << "LX: Insert obj: " << dstObjId << ". Erase obj: " << srcObjId;  
                //DEDUP_LOG() << "dst_sha1: " << dstObj["sha1"].String() 
                //    << "  src_sha1: " << srcObj["sha1"].String() 
                //    << "  storedBytes: " << (numUnqBytes + matchSeg.size() * sizeof(Segment));

                diskAccesses++;
                deltaComputeMicros += timer->micros();
                timer->reset();
            } 
            else if (indexCode == 2) {
unique:
                uniqueBlobs++;
                uniqueBytes += len;
                storedBytes += len;

                cIndex.set(cHash, dLoc, numFeatures);
                numMilestones++;
                numIndexes += numFeatures;
                DEDUP_DEBUG() << "Insert index, numFeatures:" << numFeatures << ", total indexes: " << numIndexes;
                // put into cache
                std::string dstObjId = oidE.OID().toString();
                objCache[dstObjId] = obj.getOwned();
                addToLRU(dstObjId);
                DEDUP_DEBUG() << "LX: Insert obj: " << dstObjId;  
            }
            DEDUP_DEBUG() << "IndexCode: " << indexCode;

            totalBlobs++;
            totalBytes += len;
            totalChunks += features.size();
            sampledChunks += numFeatures;
            deltaMicros = deltaFetchMicros + deltaComputeMicros; 
            elapsedMicros = chunkMicros + sampleMicros + indexMicros + deltaMicros;

            delete timer;
            return indexCode;
        }

        void PDedup::getSrcObj( 
                const std::string &ns, const OID &srcOID, BSONObj &srcObj,
                const std::string &self, const std::string &syncTarget)
        {
            if(getObjFromOid(self, srcOID, ns, srcObj) == -1) {
                DEDUP_LOG() << "getSrcObj: Cannot retrive src obj from self!";
                if (!syncTarget.empty()) {
                    while(getObjFromOid(syncTarget, srcOID, ns, srcObj) == -1) {
                        sleepmillis(5);
                        DEDUP_LOG() << "getSrcObj: Cannot retrive src obj from sync target!";
                    }
                }
            }
        }

        int PDedup::dedupBSON(const std::string &ns, const BSONObj &obj, BSONObj &newobj)
        {
            // Currently assume that "_id" exist in the original object
            BSONElement eoid = obj["_id"];
            if (eoid.eoo())
                return -1;

            // size-based filter
            if (obj.objsize() < 30 * 1024) {
                newobj = obj;
                return 0;
            }

            DEDUP_DEBUG() << "LX: dedupBSON: ns: " << ns;
            BSONObjBuilder bbld;
            bbld.append(eoid);
            BufBuilder &bb = bbld.bb();

            DiskLoc dstLoc(ns, eoid.OID()), srcLoc;
            std::vector<mongo::dedup::Segment> matchSeg;
            std::vector<unsigned char> unqData;
            bool setIndex = true;

            int ret = processBlob(obj, dstLoc, srcLoc, matchSeg, unqData, setIndex);

            std::string srcNs(srcLoc.ns);
            // binData length
            int binLen = 0;

            if (ret == 0) {
                DEDUP_DEBUG() << "LX: Whole blob duplicate: srcLoc oid: " << srcLoc.oid.toString();
                // whole blob duplicate
                // Dedup type, source doc OID, source namespace
                binLen = 1 + 12 + (srcNs.size() + 1);

                // bindata element
                bb.appendNum((char) BinData);
                bb.appendStr("dedup_data");
                bb.appendNum(binLen);
                bb.appendNum( (char) bdtCustom);
                bb.appendNum( (char) mongo::dedup::WHOLE_DUP );
                bb.appendBuf( (void *) &srcLoc.oid, 12);
                bb.appendStr(srcNs);
                newobj = bbld.obj();
            } 
            else if (ret == 1) {
                //LOG(0) << "LX: Partial duplicate. Source oid: " << srcLoc.oid.toString() << ". Dst oid: " << dstLoc.oid.toString();

                // similar blob
                // Dedup type, source blob OID, number of segments, 
                // segments data, length of unique data, unique data
                binLen = 1 + 12 +  (srcNs.size() + 1) + sizeof(int) + matchSeg.size() * 
                    sizeof(mongo::dedup::Segment) + sizeof(int) + unqData.size();

                // bindata element
                bb.appendNum((char) BinData);
                bb.appendStr("dedup_data");
                bb.appendNum(binLen);
                bb.appendNum( (char) bdtCustom);

                // actual binary data
                bb.appendNum( (char)mongo::dedup::PARTIAL_DUP );
                bb.appendBuf( (void *) &srcLoc.oid, 12);
                bb.appendStr(srcNs);
                bb.appendNum( (int) matchSeg.size());

                for (int k = 0; k < (int) matchSeg.size(); ++k) {
                    bb.appendNum((char) matchSeg[k].type);
                    bb.appendNum(matchSeg[k].offset);
                    bb.appendNum(matchSeg[k].len);
                }
                bb.appendNum( (int) unqData.size() );
                if(unqData.size() > 0) {
                    bb.appendBuf(&unqData[0], unqData.size());
                }

                newobj = bbld.obj();                

            } else if (ret == 2) {
                //LOG(0) << "LX: Unique blob.";
                // completely unique blob
                newobj = obj;
            }

            printStats();
            profile();

            return ret;
        }



        /*
           void PDedup::restoreBSON(const BSONObj &obj, BSONObj &newobj, 
           const std::string &syncTarget, const std::string &self)
           {
           BSONElement dd = obj["dedup_data"];
        // Not deduplicated, no need to reconstruct.
        if(dd.eoo()) {
        DEDUP_DEBUG() << "restoreBSON: unique doc.";
        newobj = obj;
        return;
        }

        int binLen;
        char *bindata = const_cast<char *> (dd.binData(binLen));
        mongo::dedup::DupType dupType = (mongo::dedup::DupType) *bindata;

        massert(20003, "DupType error.", 
        dupType == mongo::dedup::WHOLE_DUP || 
        dupType == mongo::dedup::PARTIAL_DUP);

        if (dupType == mongo::dedup::WHOLE_DUP) {
        mongo::OID srcOID = *(reinterpret_cast<mongo::OID *> (bindata + 1));
        std::string ns(bindata + 1 + 12);
        DEDUP_LOG() << "restoreBSON: whole duplicate. binLen: " << binLen
        << " src oid: " << srcOID.toString() << " ns: " << ns;

        getSrcObj(ns, srcOID, newobj, self, syncTarget);
        } 

        else if (dupType == mongo::dedup::PARTIAL_DUP)  {
        DEDUP_LOG() << "LX: restoreBSON: partial duplicate";
        int binOffset = 1;
        int dstLen = 0;
        mongo::OID srcOID = *(reinterpret_cast<mongo::OID *> (bindata + binOffset));
        binOffset += sizeof(mongo::OID);
        std::string ns(bindata + binOffset);
        binOffset += ns.size() + 1;
        int numMatchSegs = *(reinterpret_cast<int *> (bindata + binOffset));
        binOffset += sizeof(int);

        DEDUP_LOG() << "LX: produce: ns: " << ns << ". numMatchSegs: " << numMatchSegs << ". srcOID: " << srcOID.toString();

        std::vector<mongo::dedup::Segment> matchSeg;
        for(int k = 0; k < numMatchSegs; ++k) {
        matchSeg.push_back( *(reinterpret_cast<mongo::dedup::Segment *> 
        (bindata + binOffset)) );
        binOffset += sizeof(mongo::dedup::Segment);
        dstLen += matchSeg.back().len;
        }

        // dstLen already includes the length of unique data in matchSeg
        int unqDataLen = *(reinterpret_cast<int *> (bindata + binOffset));
        binOffset += sizeof(int);
        char *unqData = unqDataLen > 0 ? (bindata + binOffset) : NULL;

        BSONObj srcObj;
        getSrcObj(ns, srcOID, srcObj, self, syncTarget);
        boost::scoped_array<char> dst( new char [dstLen] );
        deltaDeCompress(srcObj.objdata(), dst.get(), unqData, matchSeg);
        newobj = BSONObj(dst.get()).copy();
        }

        return;
        }
        */




        VDedup::VDedup(int64_t datasetSize, int64_t avgChkSize, 
                int64_t chunkBufSize, std::string flashFileName) :
            dataSize (datasetSize),
            avgChunkSize (avgChkSize),
            rChunk (avgChkSize >> 4, avgChkSize << 4,
                    avgChkSize, chunkBufSize),
            cIndex (flashFileName, datasetSize/avgChkSize),
            chunkMicros(0.0),
            sha1Micros(0.0),
            indexMicros(0.0) {
            }

        std::string VDedup::algName()    
        {
            return std::string("VDedup");
        }

        void VDedup::profile()
        {
            if(totalBlobs % 100 == 0) {
                DEDUP_LOG() << "Total time: " << elapsedMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Chunk time: " << chunkMicros / 1000000 << " seconds.";
                DEDUP_LOG() << "Sha1 time: " << sha1Micros / 1000000 << " seconds.";
                DEDUP_LOG() << "Index time: " << indexMicros / 1000000 << " seconds.";
            }

        }

        int VDedup::processBlob(
                unsigned char *bytes,
                int len,
                DiskLoc &dLoc,
                DiskLoc &sLoc,
                bool setIndex) 
        {
            int i;
            Timer *timer = new Timer();
            std::vector<int64_t> chunkOffset, chunkLen;
            log() << "vDedup" << endl;

            rChunk.rabinChunk(bytes, len, chunkOffset, chunkLen);
            chunkMicros += timer->micros();
            timer->reset();

            for (i = 0; i < chunkOffset.size(); ++i) {
                ChunkHash cHash;
                sha1(bytes+chunkOffset[i], chunkLen[i], cHash.sha1);
                sha1Micros += timer->micros();
                timer->reset();

                // For now, objCache isn't used in VDedup indexing 
                objMap objCache;
                MetaData md;
                int indexCode = cIndex.index(cHash, 0, dLoc, md, setIndex, objCache);
                sLoc = md.dl;
                indexMicros += timer->micros();
                timer->reset();

                if (indexCode == 0) {
                    dupBlobs++;
                    dupBytes += chunkLen[i];
                }

                else if (indexCode == 2) {
                    uniqueBlobs++;
                    uniqueBytes += chunkLen[i];
                    storedBytes += chunkLen[i];
                }

                totalBlobs++;
                totalBytes += chunkLen[i];
            }
            elapsedMicros = chunkMicros + sha1Micros + indexMicros;
            delete timer;            
            return 0;
        }

        VDedup::~VDedup()
        {
        }

        void VDedup::printStats()
        {
            DedupAlg::printStats();
        }


    }
}

