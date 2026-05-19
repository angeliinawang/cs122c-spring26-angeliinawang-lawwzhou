#include "src/include/ix.h"

namespace PeterDB {
    IndexManager &IndexManager::instance() {
        static IndexManager _index_manager = IndexManager();
        return _index_manager;
    }

    RC IndexManager::createFile(const std::string &fileName) {
        auto &pfm = PagedFileManager::instance();
        if (pfm.createFile(fileName) != 0) return -1;

        FileHandle fh;
        if (pfm.openFile(fileName, fh) != 0) return -1;

        char hidden[PAGE_SIZE] = {0};
        PageNum rootPage = 0;
        memcpy(hidden, &rootPage, sizeof(PageNum)); // counters are byte 4-15, already zeroed

        if (fh.appendPage(hidden) != 0) {
            pfm.closeFile(fh);
            pfm.destroyFile(fileName);
            return -1;
        }

        return pfm.closeFile(fh);
    }

    RC IndexManager::destroyFile(const std::string &fileName) {
        return PagedFileManager::instance().destroyFile(fileName);
    }

    RC IndexManager::openFile(const std::string &fileName, IXFileHandle &ixFileHandle) {
        if (ixFileHandle.isOpen) return -1;

        auto &pfm = PagedFileManager::instance();
        if (pfm.openFile(fileName, ixFileHandle.fileHandle) != 0) return -1;

        ixFileHandle.isOpen = true;
        return ixFileHandle.readHiddenPage();
    }

    RC IndexManager::closeFile(IXFileHandle &ixFileHandle) {
        if (!ixFileHandle.isOpen) return -1;

        if (ixFileHandle.writeHiddenPage() != 0) return -1;

        auto &pfm = PagedFileManager::instance();
        if (pfm.closeFile(ixFileHandle.fileHandle) != 0) return -1;

        ixFileHandle.isOpen = false;
        return 0;
    }

    char* findInsertPos(const Attribute &attribute, const void *key, char *dataStart, int numKeys) {
        if (attribute.type == TypeVarChar) {
            char *dataptr = dataStart;
            int len;
            memcpy(&len, key, 4);
            for (int i = 0; i < numKeys; i++) {
                int curLen;
                memcpy(&curLen, dataptr, 4);
                int minLen;
                if (len < curLen) {
                    minLen = len;
                }
                else {
                    minLen = curLen;
                }
                int comparison = memcmp((char*)key + 4, dataptr + 4, minLen);
                if (comparison < 0) {
                    return dataptr;
                }
                else if (comparison == 0) {
                    if (len < curLen) {
                        return dataptr;
                    }
                }
                dataptr += 4 + curLen + RID_SIZE;
            }
            return dataptr;
        }
        else {
            for (int i = 0; i < numKeys; i++) {
                if (attribute.type == TypeInt) {
                    int newKey = *(int*)key;
                    int currentKey = *(int*)(dataStart + i * (KEY_SIZE + RID_SIZE));
                    if (newKey < currentKey) {
                        return dataStart + i * (KEY_SIZE + RID_SIZE);
                    }
                }
                else if (attribute.type == TypeReal) {
                    float newKey = *(float*)key;
                    float currentKey = *(float*)(dataStart + i * (KEY_SIZE + RID_SIZE));
                    if (newKey < currentKey) {
                        return dataStart + i * (KEY_SIZE + RID_SIZE);
                    }
                }
            }
            return dataStart + numKeys * (KEY_SIZE + RID_SIZE);
        }
    }

    RC insertWithSpaceLeaf(IXFileHandle &ixFileHandle, const Attribute &attribute, const PageNum &pageNum, const void *key, const RID &rid, char (&page)[PAGE_SIZE],
 int numKeys, int freeSpaceOffset) {
        char *dataStart = page + METADATA_SIZE;
        int entrySize;
        if (attribute.type == TypeVarChar) {
            char *insertptr = findInsertPos(attribute, key, dataStart, numKeys);
            int len;
            memcpy(&len, key, 4);
            entrySize = len + 4 + RID_SIZE;
            char *dataEnd = page + freeSpaceOffset;
            int bytes = dataEnd - insertptr;
            memmove(insertptr + entrySize, insertptr, bytes);
            memcpy(insertptr, key, entrySize - RID_SIZE); 
            memcpy(insertptr + entrySize - RID_SIZE, &rid.pageNum, 4);
            memcpy(insertptr + entrySize - RID_SIZE + 4, &rid.slotNum, 2);
        }
        else {
            entrySize = KEY_SIZE + RID_SIZE;
            char *insertptr = findInsertPos(attribute, key, dataStart, numKeys);
            char *dataEnd = page + freeSpaceOffset;
            int bytes = dataEnd - insertptr;
            memmove(insertptr + entrySize, insertptr, bytes);
            memcpy(insertptr, key, 4);
            memcpy(insertptr + 4, &rid.pageNum, 4);
            memcpy(insertptr + 8, &rid.slotNum, 2);
        }
        // update page metadata
        numKeys += 1;
        freeSpaceOffset += entrySize;
        memcpy(page + 4, &numKeys, 4);
        memcpy(page + 8, &freeSpaceOffset, 4);
        ixFileHandle.writePage(pageNum, page);
        return 0;
    }

    RC insertSplitLeaf(IXFileHandle &ixFileHandle, const Attribute &attribute, const PageNum &pageNum, const void *key, const RID &rid, char (&page)[PAGE_SIZE],
 int numKeys, int freeSpaceOffset, void *&newChildKey, PageNum &newChildPage) {
    // algo from textbook
    // split the page into two halves, will go on separate pages
    char buffer[PAGE_SIZE * 2];
    memcpy(buffer, page + METADATA_SIZE, freeSpaceOffset - METADATA_SIZE);
    char *dataStart = buffer;
    int entrySize;
    // copy every entry into the buffer and then insert the new shit into it
    if (attribute.type == TypeVarChar) {
        int len;
        memcpy(&len, key, 4);
        entrySize = len + 4 + RID_SIZE;
    }
    else {
        entrySize = KEY_SIZE + RID_SIZE;
    }
    char *insertptr = findInsertPos(attribute, key, dataStart, numKeys);
    char *dataEnd = buffer + (freeSpaceOffset - METADATA_SIZE);
    int bytes = dataEnd - insertptr;
    memmove(insertptr + entrySize, insertptr, bytes);
    if (attribute.type == TypeVarChar) {
        memcpy(insertptr, key, entrySize - RID_SIZE);
        memcpy(insertptr + entrySize - RID_SIZE, &rid.pageNum, 4);
        memcpy(insertptr + entrySize - RID_SIZE + 4, &rid.slotNum, 2);
    }
    else {
        memcpy(insertptr, key, 4);
        memcpy(insertptr + 4, &rid.pageNum, 4);
        memcpy(insertptr + 8, &rid.slotNum, 2);
    }
    // now do the splitting
    int numEntries = numKeys + 1;
    int numLeft = numEntries / 2;
    int numRight = numEntries - numLeft;
    char newPage[PAGE_SIZE] = {0};
    char *splitptr;
    // get bytes so we know how much of buffer to copy into new pages
    int bytesLeft;
    int bytesRight;
    if (attribute.type == TypeVarChar) {
        splitptr = buffer;
        for (int i = 0; i < numLeft; i++) {
            int curLen;
            memcpy(&curLen, splitptr, 4);
            splitptr += 4 + curLen + RID_SIZE;
        }
        bytesLeft = splitptr - buffer;
        bytesRight = dataEnd + entrySize - splitptr; // add entry size because dataend was before we inserted, now we have + 1 entry
    }
    else {
        bytesLeft = numLeft * entrySize;
        bytesRight = numRight * entrySize;
        splitptr = buffer + bytesLeft;
    }
    // first d entries stay
    memcpy(page + METADATA_SIZE, buffer, bytesLeft);
    memcpy(newPage + METADATA_SIZE, buffer + bytesLeft,  bytesRight);

    int node = LEAF_NODE;
    int next;
    int newPageFreeSpaceOffset = PAGE_METADATA + bytesRight;
    int oldPageFreeSpaceOffset = PAGE_METADATA + bytesLeft;

    memcpy(&next, page + 12, 4);
    memcpy(newPage, &node, 4);
    memcpy(newPage + 4, &numRight, 4);
    memcpy(newPage + 8, &newPageFreeSpaceOffset, 4);
    memcpy(newPage + 12, &next, 4);
    
    ixFileHandle.appendPage(newPage);
    next = ixFileHandle.getNumberOfPages() - 1;

    memcpy(page + 4, &numLeft, 4);
    memcpy(page + 8, &oldPageFreeSpaceOffset, 4);
    memcpy(page + 12, &next, 4);
    ixFileHandle.writePage(pageNum, page);

    // pass back up
    if (attribute.type == TypeVarChar) {
        int splitKeyLen;
        memcpy(&splitKeyLen, splitptr, 4);
        newChildKey = new char[4 + splitKeyLen];
        memcpy(newChildKey, splitptr, 4 + splitKeyLen);
    }
    else {
        newChildKey = new char[KEY_SIZE];
        memcpy(newChildKey, splitptr, KEY_SIZE);
    }
    
    // last d entries move to new page
    // new child entry is the smallest key value on the second page
    // set leaf pointers

 }

    RC insert(IXFileHandle &ixFileHandle, PageNum pageNum, const Attribute &attribute, const void *key, const RID &rid, void *&newChildKey, PageNum &newChildPage) {
        // following the algorithm from the slides
        // if the node is a leaf node
        char page[PAGE_SIZE];
        ixFileHandle.readPage(pageNum, page);
        int flag;
        int numKeys;
        int freeSpaceOffset;
        int next;
        char *pageptr = page;
        memcpy(&flag, pageptr, 4);
        memcpy(&numKeys, pageptr + 4, 4);
        memcpy(&freeSpaceOffset, pageptr + 8, 4);
        memcpy(&next, pageptr + 12, 4);
        if (flag == LEAF_NODE) {
            // differentiate ints and reals vs varchars to calculate space needed
            int keySize; 
            if (attribute.type == TypeVarChar) {
                memcpy(&keySize, key, 4);
                keySize += 4;
            }
            else {
                keySize = 4;
            }
            // if there is space
            if (keySize + freeSpaceOffset + RID_SIZE <= PAGE_SIZE) {
                // insert the the value into the correct spot and shift everything else over
                if (insertWithSpaceLeaf(ixFileHandle, attribute, pageNum, key, rid, page, numKeys, freeSpaceOffset) != 0) return -1;
            }
            // if there's NO SPACE SPLIT and pass back up
            else {
                if (insertSplitLeaf(ixFileHandle, attribute, pageNum, key, rid, page, numKeys, freeSpaceOffset, newChildKey, newChildPage) != 0) return -1;
            }
        }
        // if the node is an internal node
            // keep going down until you find the correct leaf node to insert into
            // if newChildKey and newChildPage come back with values, you have to insert that into this internal node
        else {
            // choose subtree
            // recursively insert
            // check childEntry in case of split below

        }
        return 0;

    }

    RC
    IndexManager::insertEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid) {
        // first if our tree is empty we have to update the root
        if (ixFileHandle.rootPageNum == 0) {
            char newRootPage[PAGE_SIZE] = {0};
            int node = LEAF_NODE;
            int numKeys = 0;
            int freeSpaceOffset = METADATA_SIZE;
            int next = 0;
            char *pageptr = newRootPage;
            memcpy(pageptr, &node, 4);
            memcpy(pageptr + 4, &numKeys, 4);
            memcpy(pageptr + 8, &freeSpaceOffset, 4);
            memcpy(pageptr + 12, &next, 4);
            
            ixFileHandle.appendPage(newRootPage);
            ixFileHandle.rootPageNum = ixFileHandle.getNumberOfPages() - 1;
        }
        // traverse from the root to the leaf where we can insert
        void *newChildKey = nullptr;
        PageNum newChildPage = 0;
        // insert will recursively go down until it finds a spot to place, if there is a split it handles it
        insert(ixFileHandle, ixFileHandle.rootPageNum, attribute, key, rid, newChildKey, newChildPage);

        // if newChildKey and newChildPage come back with values it means we need to create a new root since it got split
        if (newChildKey != nullptr) {
            
        }
        return 0;
    }

    RC del(IXFileHandle &ixFileHandle, PageNum pageNum, const Attribute &attribute, const void *key, const RID &rid, bool &underflow) {
        char page[PAGE_SIZE];
        if (ixFileHandle.readPage(pageNum, page) != 0) return -1;

        int flag, numKeys, freeSpaceOffset, next;
        memcpy(&flag, page, 4);
        memcpy(&numKeys, page + 4, 4);
        memcpy(&freeSpaceOffset, page + 8, 4);
        memcpy(&next, page + 12, 4);

        // if pageNum page is a leaf,
        if (flag == LEAF_NODE) {
            int entryOffset = findLeafEntry(attribute, page, numKeys, key, rid);
            if (entryOffset < 0) return -1; // entry not found

            int entrySize = leafEntrySize(attribute, page + entryOffset); // # of bytes to remove
            int remBytes = freeSpaceOffset - (entryOffset + entrySize);
            memmove(page + entryOffset, page + entryOffset + entrySize, remBytes); // shift entries left, decrement numkeys and spaceoffset, write page

            numKeys -= 1;
            freeSpaceOffset -= entrySize;
            memcpy(page + 4, &numKeys, 4);
            memcpy(page + 8, &freeSpaceOffset, 4);

            if (ixFileHandle.writePage(pageNum, page) != 0) return -1;
            // if numkeys < min, signal underflow to caller
            if (freeSpaceOffset - METADATA_SIZE < MIN_DATA_BYTES) underflow = true;

            return 0;
        }
        
        // internal node
        // find which child covers key K, recurse into it
        int childIdx = findInternalChildIds(attribute, page, numKeys, key);
        PageNum childPage;
        int childPtrOffset = METADATA_SIZE; // offset of the idx-th child pointer
        if (attribute.type == TypeVarChar) {
            if (childIdx == 0) {
                childPtrOffset = METADATA_SIZE;
            } else {
                char *p = page + METADATA_SIZE + 4; // skip ptr0
                for (int i = 0; i < childIdx; i++) {
                    int currentLen;
                    memcpy(&currentLen, p, 4);
                    p += 4 + currentLen + 4; // skip this key + next pointer
                }
                childPtrOffset = METADATA_SIZE + childIdx * (KEY_SIZE + 4);
            }
        }

        memcpy(&childPage, page + childPtrOffset, 4);

        // RECURSIVE PART
        bool childUnderflow = false;
        if (del(ixFileHandle, childPage, attribute, key, rid, childUnderflow) != 0) return -1;
        if (!childUnderflow) return 0; // we done, no redistribution needed
        
        // if child signaled underflow
        // if (redistributeOrMergeEntries(ixFileHandle, attribute, page, pageNum, numKeys, freeSpaceOffset, childIdx) != 0) return -1;
        // // if internal node is now below half full, signal up
        // memcpy(&numKeys, page + 4, 4);
        // memcpy(&freeSpaceOffset, page + 8, 4);

        if (freeSpaceOffset - METADATA_SIZE < MIN_DATA_BYTES) underflow = true;
                    // merge with sibling and remove into separate key from this node
                    // if removing key makes node drop below win, signal underflow upwards
        return 0;
    }

    // this is hell
    // RC redistributeOrMergeEntries(IXFileHandle &ixFileHandle, const Attribute &attribute, char *parentPage, PageNum parentPageNum,
    //                         int parentNumKeys, int parentFreeSpaceOffset, int childIdx) {
        
    //     char page[PAGE_SIZE];
    //     if (ixFileHandle.readPage(childIdx, page) != 0) return -1;
    // }

    RC leafEntrySize(const Attribute &attribute, const char *entry) {
        if (attribute.type == TypeVarChar) { // cannot precompute for varchars
            int len;

            memcpy(&len, entry, 4);
            return len + RID_SIZE + 4;
        }

        return KEY_SIZE + RID_SIZE;
    }

    RC findLeafEntry(const Attribute & attribute, char *page, int numKeys, const void *key, const RID &rid) {
        char *start = page + METADATA_SIZE;
        char *p = start;
        for (int i = 0; i < numKeys; i++) {
            int entrySize = leafEntrySize(attribute, p);
            bool matchingKey = false;

            if (attribute.type == TypeVarChar) {
                int currentLen, targetLen;
                memcpy(&currentLen, p, 4);
                memcpy(&targetLen, key, 4);

                if (currentLen == targetLen && memcmp(p + 4, (char*)key + 4, currentLen) == 0) {
                    matchingKey = true;
                } else {
                    if (memcmp(p, key, 4) == 0) {
                        matchingKey = true;
                    }
                }
            } else {
                if (memcmp(p, key, 4) == 0) matchingKey = true;
            }

            if (matchingKey) {
                unsigned ridPage;
                unsigned short ridSlot;
                memcpy(&ridPage, p + entrySize - RID_SIZE, 4);
                memcpy(&ridSlot, p + entrySize - RID_SIZE + 4, 2);
                if (ridPage == rid.pageNum && ridSlot == (unsigned short)rid.slotNum) {
                    return (int)(p - page);
                }
            }

            p += entrySize;
        }

        return -1;
    }

    // reads child pointer instead of using index
    RC findInternalChildIds(const Attribute &attribute, char *page, int numKeys, const void *key) {
        char *p = page + METADATA_SIZE + 4; // skip flags/numKeys/fso/next, then skip first child ptr

        for (int i = 0; i < numKeys; i++) {
            int keyLen = (attribute.type == TypeVarChar) ? (*(int*)p + 4) : 4;
            int cmp;

            if (attribute.type == TypeVarChar) {
                int currentLen, targetLen;
                memcpy(&currentLen, p, 4);
                memcpy(&targetLen, key, 4);
                int minLen = (currentLen < targetLen) ? currentLen : targetLen;
                cmp = memcmp(p + 4, (char*)key + 4, minLen);

                if (cmp == 0) {
                    cmp = (targetLen < currentLen) ? -1 : (targetLen > currentLen ? 1 : 0);
                }
            } else if (attribute.type == TypeInt) {
                int currKey = *(int*)p;
                int newKey = *(int*)key;
                cmp = (newKey < currKey) ? -1 : (newKey > currKey ? 1 : 0);
            } else { // TypeReal
                float currKey = *(float*)p;
                float newKey = *(float*)key;
                cmp = (newKey < currKey) ? -1 : (newKey > currKey ? 1 : 0);
            }
            
            if (cmp < 0) { // targetKey < currSeparator, take the child pointer STRICTLY before dat one (which is i)
                return i;
            }
            p += keyLen + 4; // go to the next key + child pointer after that
        }
            return numKeys;
    }

    RC
    IndexManager::deleteEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid) {
        // check if tree is empty, if so don't delete
        if (ixFileHandle.rootPageNum == 0) return -1;
        
        // traverse from root to find the leaf node
        bool underflow = false;
        if (del(ixFileHandle, ixFileHandle.rootPageNum, attribute, key, rid, underflow) != 0) {
            return -1;
        }
        // delete the entry
        char rootBuf[PAGE_SIZE];
        if (ixFileHandle.readPage(ixFileHandle.rootPageNum, rootBuf) != 0) return -1;

        int nodeType, numKeys;
        memcpy(&nodeType, rootBuf, 4);
        memcpy(&numKeys, rootBuf + 4, 4);

        if (nodeType == LEAF_NODE && numKeys == 0) {
            ixFileHandle.rootPageNum = 0;
        } else if (nodeType == INTERNAL_NODE && numKeys == 0) {
            PageNum onlyChild;
            memcpy(&onlyChild, rootBuf + METADATA_SIZE, sizeof(PageNum));
            ixFileHandle.rootPageNum = onlyChild;
        }
        
        return 0;
    }

    RC IndexManager::scan(IXFileHandle &ixFileHandle,
                          const Attribute &attribute,
                          const void *lowKey,
                          const void *highKey,
                          bool lowKeyInclusive,
                          bool highKeyInclusive,
                          IX_ScanIterator &ix_ScanIterator) {
        return -1;
    }

    RC IndexManager::printBTree(IXFileHandle &ixFileHandle, const Attribute &attribute, std::ostream &out) const {
        return 0;
    }

    IX_ScanIterator::IX_ScanIterator() {
    }

    IX_ScanIterator::~IX_ScanIterator() {
    }

    RC IX_ScanIterator::getNextEntry(RID &rid, void *key) {
        return -1;
    }

    RC IX_ScanIterator::close() {
        return -1;
    }

    IXFileHandle::IXFileHandle() {
        ixReadPageCounter = 0;
        ixWritePageCounter = 0;
        ixAppendPageCounter = 0;

        rootPageNum = 0;
        isOpen = false;
    }

    IXFileHandle::~IXFileHandle() = default;

    RC IXFileHandle::collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount) {
        readPageCount = ixReadPageCounter;
        writePageCount = ixWritePageCounter;
        appendPageCount = ixAppendPageCounter;
        return 0;
    }

    RC IXFileHandle::readPage(PageNum pageNum, void *data) {
        if (fileHandle.readPage(pageNum, data) != 0) return -1;
        
        ixReadPageCounter++;
        return 0;
    }

    RC IXFileHandle::writePage(PageNum pageNum, const void *data) {
        if (fileHandle.writePage(pageNum, data) != 0) return -1;

        ixWritePageCounter++;
        return 0;
    }

    RC IXFileHandle::appendPage(const void *data) {
        if (fileHandle.appendPage(data) != 0) return -1;

        ixAppendPageCounter++;
        return 0;
    }

    unsigned IXFileHandle::getNumberOfPages() {
        return fileHandle.getNumberOfPages();
        // get PFM's count because IX also treats page 0 as hidden page
    }

    RC IXFileHandle::readHiddenPage() {
        char buf[PAGE_SIZE];
        if (fileHandle.readPage(0, buf) != 0) return -1;

        memcpy(&rootPageNum, buf, sizeof(PageNum));
        memcpy(&ixReadPageCounter, buf + sizeof(PageNum), sizeof(unsigned));
        memcpy(&ixWritePageCounter, buf + sizeof(PageNum) + 4, sizeof(unsigned));
        memcpy(&ixAppendPageCounter, buf + sizeof(PageNum) + 8, sizeof(unsigned));
        return 0;
    }

    RC IXFileHandle::writeHiddenPage() {
        char buf[PAGE_SIZE] = {0};
        memcpy(buf, &rootPageNum, sizeof(PageNum));
        memcpy(buf + sizeof(PageNum), &ixReadPageCounter, sizeof(unsigned));
        memcpy(buf + sizeof(PageNum) + 4, &ixWritePageCounter, sizeof(unsigned));
        memcpy(buf + sizeof(PageNum) + 8, &ixAppendPageCounter, sizeof(unsigned));
        return fileHandle.writePage(0, buf);
    }

} // namespace PeterDB