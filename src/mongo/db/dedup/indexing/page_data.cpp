//#include <assert.h>
#include <string.h>
#include "page_data.h"

namespace mongo{
	namespace dedup
	{
        bool emptyFeature(unsigned char *feature)
        {
            int i;
            for(i = 0; i < FEATURE_LENGTH; ++i)
                if (feature[i] != 0)
                    break;
            return (i == FEATURE_LENGTH);
        }

		std::string byte2String(const unsigned char *bytes, int len)
		{
			char str[2 * SHA1_LENGTH + 1];
			for (int i = 0; i < len; ++i) {
				snprintf(str + 2 * i, 3, "%02X", bytes[i]);
			}
			return std::string(str);
		}


		std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
			std::stringstream ss(s);
			std::string item;
			while (std::getline(ss, item, delim)) {
				elems.push_back(item);
			}
			return elems;
		}


		std::vector<std::string> split(const std::string &s, char delim) {
			std::vector<std::string> elems;
			split(s, delim, elems);
			return elems;
		}


		PageData::PageData(int pgSize):
			pageSize(pgSize),
			maxSlots(pgSize / sizeof(MetaData)),
			keys(maxSlots),
			values(maxSlots),
			bufferPage(pgSize) {
			resetPageMetaData();
		}


		void PageData::appendBuffer(void *data, int &offset, int len) {
			memcpy(&bufferPage[offset], data, len);
			offset += len;
		}

		void PageData::readBuffer(void *data, const void *bytes, int &offset, int len) {			
			memcpy(data, bytes, len);
			offset += len;
		}

		std::vector<unsigned char>& PageData::convertToBytes()
		{
			int i;
			int offset = 0;
			appendBuffer(&pageId, offset, sizeof(pageId));
			appendBuffer(&typeOfPage, offset, sizeof(typeOfPage));
			appendBuffer(&timeStamp, offset, sizeof(timeStamp));
			appendBuffer(&NoOfSlots, offset, sizeof(NoOfSlots));

			for (i = 0; i < NoOfSlots; i++) {
				appendBuffer(&keys[i], offset, sizeof(ChunkHash));
				appendBuffer(&values[i], offset, sizeof(DiskLoc));
			}

			//assert(offset == occupiedSpace);

			return bufferPage;
		}

		PageData* PageData::convertFromBytes(const std::vector<unsigned char>& bytes)
		{
			int i;
			int offset = 0;
			resetPageMetaData();
			readBuffer(&pageId, &bytes[offset], offset, sizeof(pageId));
			readBuffer(&typeOfPage, &bytes[offset], offset, sizeof(typeOfPage));
			readBuffer(&timeStamp, &bytes[offset], offset, sizeof(timeStamp));
			readBuffer(&NoOfSlots, &bytes[offset], offset, sizeof(NoOfSlots));			

			for (i = 0; i < NoOfSlots; i++) {
				readBuffer(&keys[i], &bytes[offset], offset, sizeof(ChunkHash));
				readBuffer(&values[i], &bytes[offset], offset, sizeof(DiskLoc));
			}

			return this;
		}

		PageData* PageData::convertFromBytes(
			const std::vector<unsigned char>& bytes, short slotId)
		{
			int i;
			int offset = 0;
			resetPageMetaData();
			readBuffer(&pageId, &bytes[offset], offset, sizeof(pageId));
			readBuffer(&typeOfPage, &bytes[offset], offset, sizeof(typeOfPage));
			readBuffer(&timeStamp, &bytes[offset], offset, sizeof(timeStamp));
			readBuffer(&NoOfSlots, &bytes[offset], offset, sizeof(NoOfSlots));

			for (i = 0; i <= slotId; i++) {
				readBuffer(&keys[i], &bytes[offset], offset, sizeof(ChunkHash));
				readBuffer(&values[i], &bytes[offset], offset, sizeof(DiskLoc));
			}

			return this;
		}

		short PageData::addToPage(ChunkHash chash, DiskLoc dLoc)
		{
			keys[NoOfSlots] = chash;
			values[NoOfSlots] = dLoc;
			NoOfSlots++;
			occupiedSpace += sizeof(MetaData);
			return (short)(NoOfSlots - 1);
		}

		bool PageData::isPageFull()
		{
			return (occupiedSpace + (int)sizeof(MetaData) > pageSize);
		}

		int PageData::sizeOfPageMetaData()
		{
			// size of PageID, TimeStamp, NoOfSlots, and typeOfPage
			return (4 * sizeof(int));
		}

		void PageData::resetPageMetaData()
		{			
			NoOfSlots = 0;
			timeStamp = 0;
			pageId = 0;
			typeOfPage = 0;
			occupiedSpace = sizeOfPageMetaData();
		}

		void PageData::setTimeStamp(int ts)
		{
			timeStamp = ts;
		}

		int PageData::getTimeStamp()
		{
			return timeStamp;
		}

		void PageData::setPageId(int pid)
		{
			pageId = pid;
		}

		int PageData::getPageId()
		{
			return pageId;
		}

		int PageData::itemsCount()
		{
			return NoOfSlots;
		}

	}
}
