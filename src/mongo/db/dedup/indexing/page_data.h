#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include "mongo/bson/oid.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
	namespace dedup
	{
		#define MAX_FILE_LENGTH 60
		#define NUM_FEATURES	8
		#define SHA1_LENGTH		20
		#define FEATURE_LENGTH	8
        #define MAX_NS_LENGTH   60
        #define PAGE_SIZE       65536 

        #define NUM_PAGE_ENTRIES    (PAGE_SIZE / sizeof(MetaData))
		#define VALID_PID(pid)	(pid != (unsigned int)(-1) && pid != (unsigned int)(-2))
		#define EMPTY_DISKLOC(dLoc)	(!dLoc.oid.isSet())
        #define LOC(bucket) ((bucket).pageId * NUM_PAGE_ENTRIES + (bucket).loc)

	    #define oidSize mongo::OID::kOIDSize 	

        extern bool verboseDedupLogging;
        extern bool verboseDedupDebugging;
        #define DEDUP_LOG() if(verboseDedupLogging) log() << "[DEDUP LOG] " 
        #define DEDUP_DEBUG() if(verboseDedupDebugging) log() << "[DEDUP DEBUG] "

        bool emptyFeature(unsigned char *feature);
	    std::string byte2String(const unsigned char *bytes, int len);


#pragma pack(1)
		struct ChunkHash {
			unsigned char sha1[SHA1_LENGTH];
			unsigned char features[NUM_FEATURES][FEATURE_LENGTH];

            ChunkHash() {
                memset(sha1, 0, SHA1_LENGTH);
                for (int i = 0; i < NUM_FEATURES; ++i) {
                    memset(features[i], 0, FEATURE_LENGTH);
                }
            }

            ChunkHash(const ChunkHash &ch) {
                memcpy(sha1, ch.sha1, SHA1_LENGTH);
                for (int i = 0; i < NUM_FEATURES; ++i) {
                    memcpy(features[i], ch.features[i], FEATURE_LENGTH);
                }
            }


            void operator=(const ChunkHash &ch) {
                memcpy(sha1, ch.sha1, SHA1_LENGTH);
                for (int i = 0; i < NUM_FEATURES; ++i) {
                    memcpy(features[i], ch.features[i], FEATURE_LENGTH);
                }
            }

            bool operator== (const ChunkHash &ch) {
                return (memcmp(sha1, ch.sha1, SHA1_LENGTH) == 0);
            }

		};
#pragma pack()

		// on-flash location
		struct Bucket {
			uint32_t pageId;
			uint16_t loc;
            uint16_t checksum;

			Bucket() {
				pageId = -1;
				loc = -1;
                checksum = 0;
			}
			
			Bucket(uint32_t pid, uint16_t l) :
				pageId (pid),
				loc (l),
                checksum (0) {
			}

            void set(uint32_t pid, uint16_t l)
            {
                pageId = pid;
                loc = l;
            }

            void set(uint32_t pid, uint16_t l, uint16_t cksum)
            {
                pageId = pid;
                loc = l;
                checksum = cksum;
            }

            void clear()
            {
                set(-1, -1, 0);
            }

			void operator = (const Bucket& b) {
                pageId = b.pageId;
                loc = b.loc;
                // Don't overwrite checksum
			}

			bool operator == (const Bucket& b) {
				return (pageId == b.pageId &&
					loc == b.loc && checksum == b.checksum);
			}

			bool operator > (const Bucket& b) {
				return ( pageId > b.pageId ||
                        (pageId == b.pageId && loc > b.loc) );
			}
		};

#pragma pack(1)
		// pointer to on-disk location
		struct DiskLoc {			
            char ns[MAX_NS_LENGTH];
            mongo::OID oid;

            DiskLoc() :
                oid() {
                ns[0] = '\0';
            }


            DiskLoc(std::string mongoNs, const mongo::OID &id) {
                memcpy(ns, mongoNs.c_str(), mongoNs.size());
                ns[mongoNs.size()] = '\0';
                oid = id;
            }
            
            DiskLoc(const DiskLoc &dLoc) {
                memcpy(ns, dLoc.ns, MAX_NS_LENGTH);
                oid = dLoc.oid;
            }
#pragma pack()
            
            void set(const DiskLoc &dLoc) {
                memcpy(ns, dLoc.ns, MAX_NS_LENGTH);
                oid = dLoc.oid;
            }
			
            void operator=(const DiskLoc& dl) {
                memcpy(ns, dl.ns, MAX_NS_LENGTH);
                oid = dl.oid;
            }

            bool operator==(const DiskLoc& dl) {
				return (oid == dl.oid);
			}
		};

        /*
		struct ChunkMeta {
			PageLoc pl;
			DiskLoc dl;

            ChunkMeta():
                pl(),
                dl(){}

			ChunkMeta(const PageLoc &pLoc) :
				pl(pLoc),
				dl() {
			}

			ChunkMeta(const PageLoc &pLoc, const DiskLoc &dLoc) :
				pl(pLoc),
				dl(dLoc) {
			}

			bool operator==(const ChunkMeta& cMeta) {
				return (pl == cMeta.pl);
			}
		};
        */
		
#pragma pack(1)
        struct MetaData {
			ChunkHash ch;
			DiskLoc dl;

            MetaData() : 
                ch(),
                dl() {}

            MetaData(const ChunkHash &chash, const DiskLoc &dloc) :
                ch (chash),
                dl (dloc) {}

            void operator=(const MetaData &md) {
                ch = md.ch;
                dl = md.dl;
            }

            bool operator==(const MetaData &md) {
                return (ch == md.ch);
            }

		};
#pragma pack()

		std::vector<std::string> &split(const std::string &s, 
			char delim, std::vector<std::string> &elems);
		// split string s with delim as delimiter
		std::vector<std::string> split(const std::string &s, char delim);

		class PageData
		{
		private:			
			int pageSize;
			int maxSlots;
			int occupiedSpace;
			// page metadata
			int pageId;
			int timeStamp;
			int NoOfSlots;
			int typeOfPage;
			std::vector<unsigned char> bufferPage; 

			int sizeOfPageMetaData();
			void appendBuffer(void *data, int &offset, int len);
			void readBuffer(void *data, const void *bytes, int &offset, int len);
		public:
			std::vector<ChunkHash> keys;
			std::vector<DiskLoc> values;

			PageData(int size);
			// bufferPage is populated, return reference to bufferPage
			std::vector<unsigned char>& convertToBytes();

			// bufferPage is not populated (or used) in the conversion
			PageData *convertFromBytes(const std::vector<unsigned char>& bytes);
			PageData *convertFromBytes(const std::vector<unsigned char>& bytes, short slotID);
			// add a chunk's hash and metadata to buffer page
			// return slot ID in the page
			short addToPage(ChunkHash chash, DiskLoc dLoc);

			// returns true if page becomes full after inserting a new entry
			bool isPageFull();
			void resetPageMetaData();

			void setTimeStamp(int ts);

			int getTimeStamp();

			void setPageId(int pid);

			int getPageId();

			int itemsCount();
		};
	}
}
