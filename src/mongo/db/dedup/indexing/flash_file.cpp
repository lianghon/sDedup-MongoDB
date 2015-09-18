#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "flash_file.h"
#include "page_data.h"
#include "mongo/util/log.h"

namespace mongo {
	namespace dedup
	{
		CustomFileIO::CustomFileIO(std::string fName, int pgSize, bool truncate) :
			fileName(fName),
			pageSize(pgSize),
			fs(fName.c_str(), (std::ios::in | std::ios::out | std::ios::binary | std::ios::app) ),
			writebufOffset(0) {
                //fs.open(fName.c_str(), (std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc));
                if ( !fs.is_open() ) {
                    DEDUP_DEBUG() << "File open failed!";
                } else {
                    DEDUP_DEBUG() << "File open succeeded!";
                }
                fs.seekg(0, fs.end);
                fileWriteOffset = fs.tellg();
                currentPage = fileWriteOffset / pageSize;
                writebufStartPage = currentPage;
                DEDUP_DEBUG() << "currentPage: " << currentPage;
                
		}

		CustomFileIO::~CustomFileIO()
		{
			fileClose();
		}

		std::vector<unsigned char> CustomFileIO::readAll()
        {
			boost::mutex::scoped_lock lk(_m);
            fs.seekg(0, fs.end);
            int length = fs.tellg();
            std::vector<unsigned char> buffer(length);
            
            if (length > 0) {
                fs.seekg(0, fs.beg);
                fs.read((char *)(&buffer[0]), length);
            }
            return buffer;
        }

		std::vector<unsigned char> CustomFileIO::fileReadPage(int pageId)
		{
			boost::mutex::scoped_lock lk(_m);
			std::vector<unsigned char> buffer(pageSize);
			//assert(pageId < currentPage);

			// read from write buffer
			if (pageId >= writebufStartPage) {
                DEDUP_DEBUG() << "LX: fileReadPage: read from write buffer.";
				int offset = (pageId - writebufStartPage) * pageSize;
				memcpy(&buffer[0], writebuf + offset, pageSize);
			} 

			// else must read from flash
			else {
                DEDUP_DEBUG() << "LX: fileReadPage: read from flash. Page ID: " << pageId;
				fs.seekg(pageId * pageSize, std::ios_base::beg);
				fs.read((char *)(&buffer[0]), pageSize);			
            }

			return buffer;
		}

		std::vector<unsigned char> CustomFileIO::fileReadPage(int pageId, int count)
		{
			boost::mutex::scoped_lock lk(_m);

			std::vector<unsigned char> buffer(pageSize * count);
			//assert(pageId + count <= currentPage);

            
			// read from write buffer
			if (pageId >= writebufStartPage) {
				int offset = (pageId - writebufStartPage) * pageSize;
				memcpy(&buffer[0], writebuf + offset, pageSize * count);
			}

			// else must read from flash
			else { 
				fs.seekg(pageId * pageSize, std::ios_base::beg);
				fs.read((char *)(&buffer[0]), pageSize * count);
			}

			return buffer;
		}

		int CustomFileIO::fileWritePage(std::vector<unsigned char> &pageBuf)
		{
			boost::mutex::scoped_lock lk(_m);
			//assert(pageBuf.size() == pageSize);
            if ( !fs.is_open() ) {
                DEDUP_DEBUG() << "File open failed!";
            } else {
                DEDUP_DEBUG() << "LX: fileWritePage, pageBuf size: " << pageBuf.size();
            }
            
			if ((writebufOffset + pageSize) > writebufSize) {
				// flush (almost) full buffer
				// do not pad end zeroes
				fs.seekp(0, std::ios_base::end);
				fs.write((char *)writebuf, writebufOffset);
                DEDUP_DEBUG() << "LX: fileWritePage: write to file, offset: " << writebufOffset;
                fs.flush();
				// advance file write offset
				fileWriteOffset += writebufOffset;
				// reset buffer
				writebufOffset = 0;
				writebufStartPage = currentPage;
			}

			// there should be room in write buffer
			if ((writebufOffset + pageSize) <= writebufSize) {
				memcpy(writebuf + writebufOffset, &pageBuf[0], pageSize);
				writebufOffset += pageSize;
				currentPage++;
                DEDUP_DEBUG() << "LX: fileWritePage: write to buffer, offset: " << writebufOffset;
				return (currentPage - 1);
			}

			DEDUP_DEBUG() << "LX: fileWritePage: Should never reach here!";
			exit(1);
			return -1;
		}

		int CustomFileIO::fileWritePage(std::vector<unsigned char> &pageBuf, int pageNo)
		{
			boost::mutex::scoped_lock lk(_m);

			fs.seekp(pageNo * pageSize, std::ios_base::beg);
			fs.write((char *)(&pageBuf[0]), pageSize);
			return pageNo;
		}

		void CustomFileIO::fileClose()
		{
			boost::mutex::scoped_lock lk(_m);
			fs.close();
		}

		int CustomFileIO::getCurrentPageId()
		{
			return currentPage;
		}

		void CustomFileIO::setCurrentPageId(int pageID)
		{
			currentPage = pageID;
		}
	}
}
