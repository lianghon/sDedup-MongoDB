#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "cuckoo_hash.h"
#include <math.h>
#include "mongo/util/log.h"

namespace mongo {
	namespace dedup{

		// 64-bit hash for 64-bit platforms
		// by Austin Appleby
		uint64_t MurmurHash64A(const void * key, int len, unsigned int seed)
		{
			const uint64_t m = 0xc6a4a7935bd1e995;
			const int r = 47;

			uint64_t h = seed ^ (len * m);

			const uint64_t * data = (const uint64_t *)key;
			const uint64_t * end = data + (len / 8);

			while (data != end)
			{
				uint64_t k = *data++;

				k *= m;
				k ^= k >> r;
				k *= m;

				h ^= k;
				h *= m;
			}

			const unsigned char * data2 = (const unsigned char*)data;

			switch (len & 7)
			{
			case 7: h ^= uint64_t(data2[6]) << 48;
			case 6: h ^= uint64_t(data2[5]) << 40;
			case 5: h ^= uint64_t(data2[4]) << 32;
			case 4: h ^= uint64_t(data2[3]) << 24;
			case 3: h ^= uint64_t(data2[2]) << 16;
			case 2: h ^= uint64_t(data2[1]) << 8;
			case 1: h ^= uint64_t(data2[0]);
				h *= m;
			};

			h ^= h >> r;
			h *= m;
			h ^= h >> r;

			return h;
		}
		


		bool isPrime(const uint64_t & n)
		{
			if (n < 2)
				return false;

			for (uint64_t i = 2; i < sqrt(n + 1); ++i) {
				if (n % i == 0)
					return false;
			}
			return true;
		}


		uint64_t nextPrime(uint64_t n)
		{
			uint64_t x = n + 1;
			while (!isPrime(x))
				++x;
			return x;
		}

		CuckooHT::CuckooHT(uint64_t numEntries, KeyType kType,
                CustomFileIO &fFile, PageData &rPage, fdMap &ramBuf, fdMap &prefCache,
                boost::unordered_map<uint32_t, int> &lruM, std::list<uint32_t> &lruL) : 
            defaultLoadFactor (0.8),
			tableSize (nextPrime( (uint64_t) (numEntries / numBuckets / defaultLoadFactor) )),
            keyType (kType),
			flashFile (fFile),
			readPage (rPage),
			ramBuffer (ramBuf),
			prefetchCache (prefCache),
            lruMap (lruM),
            lruList (lruL),
			ht (tableSize) {
		}

		uint64_t CuckooHT::getIndex(uint64_t hash1, uint64_t hash2, 
			int seed, uint16_t &checksum)
		{
			uint64_t hash = hash1 + seed * hash2;
			uint64_t index = (hash & 0xffffffff) % tableSize;
			checksum = (hash >> 32) & 0xffff;
			return index;
		}


        MetaData CuckooHT::getMetaData(const Bucket& b) 
        {
            MetaData md;
            // first try ram buffer
            fdMap::iterator it = ramBuffer.find( LOC(b) );
            if (it != ramBuffer.end()) {
                md = (*it).second;
                //DEDUP_DEBUG() << "found in ram buffer";
            } 
            else {	
                // try prefetch cache
                it = prefetchCache.find( LOC(b) );
                if (it != prefetchCache.end()) {
                    md = (*it).second;
                    //DEDUP_DEBUG() << "found in prefetch cache"; 
                } 
                else {
                    // read from flash and add to prefetch cache
                    //DEDUP_DEBUG() << "read from flash and add to prefetch cache.";
                    addToPrefetchCache(b.pageId);
                    md = prefetchCache[ LOC(b) ];
                }
            }
            return md;
        }

        bool CuckooHT::keyMatch(const unsigned char *key, const MetaData &md)
        {
            bool match = false;
            if (keyType == SHA1) {
                if (memcmp(key, md.ch.sha1, SHA1_LENGTH) == 0)
                    match = true;
            } else if (keyType == FEATURE) {
                for (int k = 0; k < NUM_FEATURES; ++k) {
                    if(memcmp(key, md.ch.features[k], FEATURE_LENGTH) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            return match;
        }
		
        void CuckooHT::addToPrefetchCache(uint32_t pageId)
		{
            DEDUP_DEBUG() << "CuckooHT::addToPrefetchCache: pageId: " << pageId;
			int i;
            boost::unordered_map<uint32_t, int>::iterator lruIt = lruMap.find(pageId);
            // if it's already in the prefetch cache, just return
            if (lruIt != lruMap.end())
               return; 
            
			// remove from prefetch cache if LRU size is exceeded
			if (lruList.size() >= LRUCount) {
				int victimPage = lruList.front();
                readPage.convertFromBytes( flashFile.fileReadPage(victimPage) );
                for (i = 0; i < readPage.itemsCount(); ++i) {
                    Bucket b(victimPage, i);
                    prefetchCache.erase( LOC(b) );
                }
                // remove victim page from LRU
                lruList.pop_front();
                lruMap.erase(victimPage);
            }
				
			// now we are gurantteed to have room in the prefetch cache
			// read the page from flash, and add all its entries into the cache
			readPage.convertFromBytes(flashFile.fileReadPage(pageId));
			for (i = 0; i < readPage.itemsCount(); ++i) {
                Bucket b(pageId, i);
                prefetchCache[ LOC(b) ] = MetaData(readPage.keys[i], readPage.values[i]);
			}

            // update LRU structure
            lruList.push_back(pageId);
            lruMap[pageId] = 1;
		}
		

        std::vector<MetaData> CuckooHT::find(const unsigned char *key, bool findAll)
		{
            boost::mutex::scoped_lock lk(_mutex);
            std::vector<MetaData> matches;

            int length = (keyType == SHA1) ? SHA1_LENGTH : FEATURE_LENGTH;
			uint64_t hash1 = MurmurHash64A(key, length / 2, 0);
			uint64_t hash2 = MurmurHash64A(key + length / 2, length / 2, 0);
		
            uint64_t index;     // index into cuckoo hash table
			uint16_t checksum;  // computed checksum

            DEDUP_DEBUG() << "CuckooHT::find: key: " << byte2String(key, length);
			// start cuckoo poking, 16 (numHashFunctions) possible indices.
			for (int seed = 0; seed < numHashFunctions; ++seed) {
				index = getIndex(hash1, hash2, seed, checksum);

                for (int i = 0; i < numBuckets; ++i) {
                    Bucket &b = ht[index][i];
                    // when hitting an empty slot, return
                    if (b.pageId == (uint32_t)(-1)) 
                        return matches;

                    if (checksum == b.checksum) {					
                        // verify key match
                        MetaData md = getMetaData(b);

                        if (keyMatch(key, md)) {
                            matches.push_back(md);
                            DEDUP_DEBUG() << "find: match: " << byte2String(md.ch.sha1, SHA1_LENGTH)
                                << ". Page ID: " << b.pageId << ". Loc: " << b.loc; 
                            if (!findAll || matches.size() >= maxMatches)
                                return matches;
                        }
                        else {
                            //DEDUP_DEBUG() << "find: key string not match.";
                        }
                    } else {
                        DEDUP_DEBUG() << "find: checksum does not match. Page ID: " << b.pageId << ". Loc: " << b.loc << ". Checksum: " << b.checksum; 
                    }

                }
			}
            // no match found
            // TODO: only return last 8 entries for one feature.
			return matches;
		}

        int CuckooHT::insert(const unsigned char *key, int pageId, int loc)
		{
            boost::mutex::scoped_lock lk(_mutex);

            int length = (keyType == SHA1) ? SHA1_LENGTH : FEATURE_LENGTH;
			uint64_t hash1 = MurmurHash64A(key, length / 2, 0);
			uint64_t hash2 = MurmurHash64A(key + length / 2, length / 2, 0);
		
            int seed;
            uint64_t index;     // index into cuckoo hash table
			uint16_t checksum;  // computed checksum

            Bucket prevBucket(pageId, loc);
            Bucket tmpBucket;
            int numMatches = 0;

			// start cuckoo poking, 16 (numHashFunctions) possible indices.
			for (seed = 0; seed < numHashFunctions; ++seed) {
				index = getIndex(hash1, hash2, seed, checksum);

                for (int i = 0; i < numBuckets; ++i) {
                    Bucket &b = ht[index][i];
                    if (checksum == b.checksum) {					
                        if ( keyMatch(key, getMetaData(b)) ) {
                            tmpBucket = b;
                            b = prevBucket;
                            prevBucket = tmpBucket;
                            numMatches++;
                            if (numMatches == maxMatches)
                                return 0;
                        }
                    }

                    // when hitting an empty slot, insert previous bucket and return 
                    if (b.pageId == (uint32_t)(-1)) {
                        b = prevBucket;
                        b.checksum = checksum;
                        DEDUP_DEBUG() << "insert: key: " << byte2String(key, length)
                            << ". Page ID: " << b.pageId << ". Loc: " << b.loc; 
                        return 0;
                    }
                }
			}
            // No empty bucket after 16 pokings, kick out a victim.
            // Currently doesn't re-insert the victim.
            seed = rand() % numHashFunctions;
            int bucketNum = rand() % numBuckets;
            index = getIndex(hash1, hash2, seed, checksum);
            
            Bucket &b = ht[index][bucketNum];
            DEDUP_DEBUG() << "kickout: " << byte2String(getMetaData(b).ch.sha1, SHA1_LENGTH) 
                << ". Page ID: " << b.pageId << ". Loc: " << b.loc; 
            
            b.set(pageId, loc, checksum);
            return -1;
		}

        int CuckooHT::remove(const unsigned char *key, const ChunkHash &ch)
		{
            boost::mutex::scoped_lock lk(_mutex);
            int length = (keyType == SHA1) ? SHA1_LENGTH : FEATURE_LENGTH;
			uint64_t hash1 = MurmurHash64A(key, length / 2, 0);
			uint64_t hash2 = MurmurHash64A(key + length / 2, length / 2, 0);
		
            uint64_t index;     // index into cuckoo hash table
			uint16_t checksum;  // computed checksum

			// start cuckoo poking, 16 (numHashFunctions) possible indices.
			for (int seed = 0; seed < numHashFunctions; ++seed) {
				index = getIndex(hash1, hash2, seed, checksum);

                for (int i = 0; i < numBuckets; ++i) {
                    Bucket &b = ht[index][i];
                    // when hitting an empty slot, nothing to remove 
                    if (b.pageId == (uint32_t)(-1)) 
                        return -1;

                    if (checksum == b.checksum) {					
                        // verify document match
                        MetaData md = getMetaData(b);

                        // The verification here is different from find()
                        if (md.ch == ch) {
                            b.clear();
                            return 0;
                        }
                    }
                }
			}
            // no match found
			return -1;
		}




/*
		void CuckooHT::insertPage(PageData &page)
		{
			int pageId = page.getPageId();
			for (int i = 0; i < page.itemsCount(); ++i) {
				PageLoc pLoc(pageId, i);
				insertSha1(page.keys[i].sha1, pLoc);

				for (int j = 0; j < NUM_FEATURES; ++j) {
                    unsigned char *feature;
                    if( !emptyFeature((feature = page.keys[i].features[j])) ) { 
					    insertFeature(feature, pLoc);
                        //DEDUP_DEBUG() << "insert feature. pageId: " << pageId << ". loc: " << i <<". " << byte2String(feature, FEATURE_LENGTH);
                    }
				}
			}
		}

		Bucket & CuckooHT::findSha1(const unsigned char sha1[])
		{
			char key[SHA1_LENGTH];
			memcpy(key, sha1, SHA1_LENGTH);

			uint64_t hash1 = MurmurHash64A(key, SHA1_LENGTH / 2, 0);
			uint64_t hash2 = MurmurHash64A(
				key + SHA1_LENGTH / 2, SHA1_LENGTH / 2, 0);
		
			uint16_t checksum1;

			// start cuckoo poking, 16 (numHashFunctions) possible indices.
			for (int seed = 0; seed < numHashFunctions; ++seed) {
				uint64_t index = getIndex(hash1, hash2, seed, checksum1);
				uint16_t checksum2 = sha1Table[index].checksum;

				// empty slot, return
				if (pgLocs[index].pageId == (uint32_t)(-1)) {
                    checksums[index] = checksum1;
					return pgLocs[index];
				}
				else if (checksum1 == checksum2) {					
					return pgLocs[index];	// end cuckoo poking
				}
			}
			
			return nonPloc;
		}

		boost::array<PageLoc, maxBuckets>& CuckooHT::findFeature(
			const unsigned char feature[])
		{
			char key[FEATURE_LENGTH];
			memcpy(key, feature, FEATURE_LENGTH);

			uint64_t hash1 = MurmurHash64A(key, FEATURE_LENGTH / 2, 0);
			uint64_t hash2 = MurmurHash64A(
				key + FEATURE_LENGTH / 2, FEATURE_LENGTH / 2, 0);

			uint16_t checksum1;		

			// start cuckoo poking, 16 (numHashFunctions) possible indices.
			for (int seed = 0; seed < numHashFunctions; ++seed) {
				uint64_t index = getIndex(hash1, hash2, seed, checksum1);
				uint16_t checksum2 = checksumsFT[index];
				
				// empty slot, key must not exist
                // TODO: check all buckets instead of only the first one
				if (pgLocsFT[index][0].pageId == (uint32_t)(-1)) {
                    checksumsFT[index] = checksum1;
					return pgLocsFT[index];
				} else if (checksum1 == checksum2) {
					// find a slot, no need to update, just return
					return pgLocsFT[index];	// end cuckoo poking
				}
			}

			return nonPlocArray;
		}

		void CuckooHT::insertSha1(unsigned char sha1[], PageLoc pLoc)
		{
			int i;
			unsigned char key[SHA1_LENGTH];
			memcpy(key, sha1, SHA1_LENGTH);

			for (i = 0; i < maxLoop; ++i) {
				PageLoc &pl = findSha1(key);
				PageLoc tmpPloc;	// scope: 1 loop

				if (pl.pageId != (uint32_t)(-2)) {
					// pl either refers to an empty slot (fill it) or 
					// an occupied slot (replace it)
					pl = pLoc;
					return;	// insertion success
				}
				else {
					//	No empty slot after cuckoo poking for 16 times
					uint64_t hash1 = MurmurHash64A(key, SHA1_LENGTH / 2, 0);
					uint64_t hash2 = MurmurHash64A(
						key + SHA1_LENGTH / 2, SHA1_LENGTH / 2, 0);
					uint16_t checksum;
					
					// randomly select an exisiting element in the cuckoo 
					// hash chain, kick it out and re-insert it
					int seed = rand() % numHashFunctions;
					uint64_t index = getIndex(hash1, hash2, seed, checksum);
					tmpPloc = pgLocs[index];
					pgLocs[index] = pLoc;

					// read from flash to get the key for the victim element 
					// to be re-inserted
					readPage->convertFromBytes(
						flashFile->fileReadPage(tmpPloc.pageId), tmpPloc.loc);

					// insertSha1(readPage->keys[tmpPloc.loc].sha1, tmpPloc);
					// not using recursion, so we can keep count of loops.
					memcpy(key, readPage->keys[tmpPloc.loc].sha1, SHA1_LENGTH);
					pLoc = tmpPloc;
				}
			}

			if (i == maxLoop) {
				// this should be really rare
				numInsertFail++;
			}
		}

		void CuckooHT::pushBackArray(
			boost::array<PageLoc, maxBuckets> &plArray,
			PageLoc pLoc)
		{
			unsigned int i, j;
			for (i = 0; i < maxBuckets; ++i) {
				if (plArray[i].pageId == (uint32_t)(-1)) {
					plArray[i] = pLoc;
					break;
				}					
			}

			// if array is full, pop front an element
			if (i == maxBuckets) {
				for (j = 0; j < maxBuckets - 1; ++j) {
					plArray[j] = plArray[j+1];
				}
				plArray[j] = pLoc;
			}
		}

		void CuckooHT::insertFeature(unsigned char feature[], PageLoc pLoc)
		{
			unsigned char key[FEATURE_LENGTH];
			memcpy(key, feature, FEATURE_LENGTH);

			boost::array<PageLoc, maxBuckets> &plArray = findFeature(key);

            // TODO: fix bug - not only check the first bucket
			if (plArray[0].pageId != (uint32_t)(-2)) {
				// pl either refers to an empty slot (fill it) or 
				// an occupied slot (replace it)
				pushBackArray(plArray, pLoc);
				return;	// insertion success
			}
			else {
				//	No empty slot after cuckoo poking for 16 times					
				
				uint64_t hash1 = MurmurHash64A(key, FEATURE_LENGTH / 2, 0);
				uint64_t hash2 = MurmurHash64A(
					key + FEATURE_LENGTH / 2, FEATURE_LENGTH / 2, 0);
				uint16_t checksum;

				// randomly select an exisiting element in the cuckoo 
				// hash chain, kick it out. No re-insertion here for
				// simplicity, might lose minor dedup quality
				int seed = rand() % numHashFunctions;
				uint64_t index = getIndex(hash1, hash2, seed, checksum);
				pgLocsFT[index][0] = pLoc;
			}

		}
 
*/
	}
}	


