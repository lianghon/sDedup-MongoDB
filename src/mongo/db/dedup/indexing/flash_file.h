#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <boost/thread/mutex.hpp>


namespace mongo{
	namespace dedup
	{
		class CustomFileIO
		{
		private:
			// for write buffering
			static const int writebufSize = 128 * 1024; // in bytes
			std::string fileName;
			int pageSize;
			std::fstream fs;
			unsigned char writebuf[writebufSize];
			int currentPage;
			int writebufStartPage;
			int writebufOffset;
			int fileWriteOffset;
			boost::mutex _m;

		public:
			CustomFileIO(std::string fName, int pgSize, bool truncate);
			~CustomFileIO();
		    
            std::vector<unsigned char> readAll();
			
            std::vector<unsigned char> fileReadPage(int pageId);

			std::vector<unsigned char> fileReadPage(int pageId, int count);

			int fileWritePage(std::vector<unsigned char> &bufPage);

			int fileWritePage(std::vector<unsigned char> &bufPage, int pageNo);

			void fileClose();

			int getCurrentPageId();

			void setCurrentPageId(int pageId);
		};
	}
}
