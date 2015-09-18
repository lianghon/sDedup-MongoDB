#include <iostream>
#include <ctime>
#include <iomanip>

#include "mongo/db/dedup/indexing/chunk_index.h"
#include "mongo/db/dedup/chunking/rabin_chunking.h"
#include "mongo/util/timer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"

namespace mongo {
	namespace dedup {

		#define DELTA_SAMPLE_INTVL  32

		enum SegType {
			DUP_SEG = 0,
			UNQ_SEG = 1
		};

        enum DupType {
            WHOLE_DUP = 0,
            PARTIAL_DUP = 1
        };

#pragma pack(1)
		class Segment 
		{
		friend class PDedup;
        public:
			char type;
			int offset;
			int len;

			Segment(char t, int off, int l) :
				type(t),
				offset(off),
				len(l) {
			}
		};
#pragma pack()

		class DedupAlg {
		protected:
			/*
			uniqueBytes only refers to size of unique blobs. storedBytes = 
			uniqueBytes + (non-compressible bytes in delta compression).
			*/
			int64_t totalBytes;	// total bytes processed
			int64_t dupBytes;	// duplicate bytes
			int64_t uniqueBytes;	// bytes of unique blobs
			int64_t storedBytes;	// bytes actually stored

			int64_t totalBlobs;
			int64_t dupBlobs;
			int64_t uniqueBlobs;
			int64_t totalChunks;
			double elapsedMicros;
			std::string name;
		public:
			DedupAlg();
            virtual ~DedupAlg() {}
			virtual std::string algName() = 0;
			//virtual void processBlob(unsigned char* bytes,
			//	int len, const DiskLoc &dLoc);
			
			virtual void printStats();
		};


        class VDedup : public DedupAlg {
        private:
            int64_t dataSize;
            int64_t avgChunkSize;
            double chunkMicros;
            double sha1Micros;
            double indexMicros;
            RabinChunking rChunk;
            ChunkIndex cIndex;
        public:
            VDedup(int64_t dataSize, int64_t avgChunkSize, 
                int64_t chunkBufferSize, std::string flashFile);
            ~VDedup();
            std::string algName();
            void printStats();
            void profile();

            int processBlob(
                unsigned char* bytes,
                int len,
                DiskLoc &dLoc,
                DiskLoc &sLoc,
                bool setIndex);
        };

		class PDedup : public DedupAlg {
		private:
			int64_t dataSize;
			int64_t avgChunkSize;
			int64_t numChunks;
			RabinChunking rChunk;
			ChunkIndex cIndex;
            objMap objCache; // source object cache
            std::list<std::string> OIDLru;  // LRU list of object ID, records insertion order
            int cacheSize; // number of cache entries
            boost::mutex _m;

            // profiling stats
			int64_t sampledChunks;
			double chunkMicros;
			double sampleMicros;
			double indexMicros;
			double deltaMicros;
			double deltaFetchMicros;
			double deltaComputeMicros;
			double deltaIndexMicros;
			double deltaMatchMicros;
            double cacheFetchMicros;
            double dbFetchMicros;
			int diskAccesses;
            int cacheLookups;
            int cacheHits;
            int numIndexes;
            int numMilestones;
		public:
			PDedup(int64_t numDocs, int64_t avgChkSize, 
                int64_t chunkBufSize, std::string fName, int cSize);
            ~PDedup();

			std::string algName();
			
			void fullRabinHash(const unsigned char *bytes, int offset, uint64_t &hash);
			void incRabinHash(const unsigned char *bytes, int offset, uint64_t &hash);

            int getObjFromOid(std::string hostAndPort, 
                const mongo::OID &srcOID, std::string ns, BSONObj &obj);

            int processBlob(
                const BSONObj &obj,
                DiskLoc &dLoc, 
                DiskLoc &sLoc,
                std::vector<Segment> &matchSeg,
                std::vector<unsigned char> &uniqueData,
                bool setIndex);
            /*
			int processBlob(
				unsigned char* bytes,
				int len,
				DiskLoc &dLoc,
                DiskLoc &sLoc,
				std::vector<Segment> &matchSeg,
				std::vector<unsigned char> &uniqueData,
                bool setIndex);
            */
			
            void printStats();
			void profile();
            void profileSecondary();

            void addToLRU(const std::string& objId);
			/*
			@return: length of matched bytes
			@param matchSeg: segments in dst that have matches in src
			*/
			int deltaCompress(
				const unsigned char *src, int srcLen,
				const unsigned char *dst, int dstLen,
				std::vector<Segment> &matchSeg,
				std::vector<unsigned char> &unqBytes);

            /*
		    void deltaDeCompress(const char *src, 
                char *dst,
                char *unqBytes,
                const std::vector<Segment> &matchSeg);
            */
            void deltaDeCompress(const BSONObj &srcObj, 
                BSONObj &dstObj,
                char *unqBytes,
                const std::vector<Segment> &matchSeg);

            void getSrcObj( 
                const std::string &ns, const OID &srcOID, BSONObj &srcObj,
                const std::string &self, const std::string &syncTarget);

            int dedupBSON(const std::string &ns, const BSONObj &obj, BSONObj &newobj);
        
            void restoreBSON(const BSONObj &obj, BSONObj &newobj, 
                const std::string &syncTarget=std::string(),
                const std::string &self=std::string("localhost:27017") );
		};
	}
}
