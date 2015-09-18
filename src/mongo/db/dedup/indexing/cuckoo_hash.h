#pragma once

#include <vector>
#include <list>
#include <boost/array.hpp>
#include <boost/unordered_map.hpp>
#include <boost/thread/mutex.hpp>

#include "page_data.h"
#include "flash_file.h"

namespace mongo
{
	namespace dedup
	{

	    typedef boost::unordered_map<uint32_t, MetaData> fdMap;  // map: flash loc to disk loc

		const static unsigned int numBuckets = 4;
        const static int WINDOW_SIZE = 16;
        const static unsigned int LRUCount = 1024;
        const static int maxFlashSizeInPages = 1000000; // flash size ~64GB

		bool isPrime(const uint64_t & n);

		// brute-force way to find the next prime number
		uint64_t nextPrime(uint64_t n);

		// 64-bit hash for 64-bit platforms
		// by Austin Appleby
		uint64_t MurmurHash64A(const void * key, int len, unsigned int seed);

        enum KeyType {
            SHA1 = 0,
            FEATURE = 1
        };


		class CuckooHT {
		private:
			const static short numHashFunctions = 16;
			const static short maxLoop = 50;
            const static int maxMatches = 8;
		    float defaultLoadFactor;
			uint64_t tableSize;					
            KeyType keyType;
            boost::mutex _mutex;
            std::string fileName;	

            // Data structures shared by both sha1 and feature tables.
			CustomFileIO & flashFile;
			PageData & readPage;
            fdMap & ramBuffer;
            fdMap & prefetchCache;
            boost::unordered_map<uint32_t, int> & lruMap;
            std::list<uint32_t> & lruList;

            // core data structure
		    std::vector<boost::array<Bucket, numBuckets> > ht;
			
            // stats
			int numInsertFail;

		public:
            CuckooHT(uint64_t numEntries, KeyType kType,
                    CustomFileIO &fFile, PageData &rPage, fdMap &ramBuf, fdMap &prefCache,
                    boost::unordered_map<uint32_t, int> &lruM, std::list<uint32_t> &lruL);

			uint64_t getIndex(uint64_t hash1, uint64_t hash2,
				int seed, uint16_t &checksum);
			
            MetaData getMetaData(const Bucket& b); 

            bool keyMatch(const unsigned char *key, const MetaData &md);
            
            void addToPrefetchCache(uint32_t pageId);
			
            std::vector<MetaData> find(const unsigned char *key, bool findAll=true);
            
            int insert(const unsigned char *key, int pageId, int loc);

            int remove(const unsigned char *key, const ChunkHash &ch);
		};
	}
}
