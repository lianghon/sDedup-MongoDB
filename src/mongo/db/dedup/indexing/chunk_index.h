#pragma once

#include <string>
#include <list>

#include "page_data.h"
#include "flash_file.h"
#include "cuckoo_hash.h"
#include "mongo/client/dbclientcursor.h"

namespace mongo {
namespace dedup {

    typedef boost::unordered_map<std::string, BSONObj> objMap;

	class ChunkIndex {
	private:
		// uint64_t hashTableSize;				
		// container is concept for disk store
		// page is concept for flash store
		// disk store is not currently implemented, so now container_id == page_id
		// TODO: need to distinguish page and container later
		//int containerIDCount;	
		//bool useSketch;

		CustomFileIO flashFile;
		PageData readPage;
		PageData writePage;
        fdMap ramBuffer;
        fdMap prefetchCache;
		// records which page data need to be evicted in the prefetch cache,
		// when prefetch cache size is exceeded.
        boost::unordered_map<uint32_t, int> lruMap; 
        std::list<uint32_t> lruList;

        CuckooHT sha1HT;
        CuckooHT featureHT;
        boost::mutex _m;
        
	public:
        bool rebuildDone;
		
        ChunkIndex(std::string fName, uint64_t numDocs);
        
        void rebuild();
		
        void addToFlashAndNewContainer();
		
		/* key: sha1 + sketch
		* value: chunk on-disk location
		*/
		void set(ChunkHash cHash, DiskLoc &dLoc, int numFeatures);

        void remove(const ChunkHash &cHash, int numFeatures);

		/*
		@return code of indexing result.
		0 - find whole blob duplicate.
		1 - no blob duplicate, but similar blobs.
		2 - no duplicate or any similar blobs.
		*/
		int index(
			const ChunkHash & cHash,
			int numFeatures,
			DiskLoc &dLoc,
			MetaData &simDoc,
            bool setIndex,
            const objMap &cache);

		void printStats();
	};
}
}
