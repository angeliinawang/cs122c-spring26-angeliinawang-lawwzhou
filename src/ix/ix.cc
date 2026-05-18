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

    RC insert(PageNum pageNum, const Attribute &attribute, const void *key, const RID &rid, void *newChildKey, PageNum newChildPage) {
        // following the algorithm from the slides

        // if the node is a leaf node
        // if there is space
        // insert the the value into the correct spot and shift everything else over
        // if there is no space, you have to split aka make a new page and move half over
        //and pass the newChildKey and newChildPage back up

        // update the metadata



        // if the node is an internal node
        // keep going down until you find the correct leaf node to insert into
        // if newChildKey and newChildPage come back with values, you have to insert that into this internal node
        // if there's a split repeat and pass back up
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
            memcpy(&newRootPage, &node, 4);
            memcpy(&newRootPage, &numKeys, 8);
            memcpy(&newRootPage, &freeSpaceOffset, 12);
            memcpy(&newRootPage, &next, 16);
            
            ixFileHandle.appendPage(newRootPage);
            ixFileHandle.rootPageNum = ixFileHandle.getNumberOfPages() - 1;
        }
        // traverse from the root to the leaf where we can insert
        void *newChildKey = nullptr;
        PageNum newChildPage = 0;
        // insert will recursively go down until it finds a spot to place, if there is a split it handles it
        insert(ixFileHandle.rootPageNum, attribute, key, rid, newChildKey, newChildPage);
        // if newChildKey and newChildPage come back with values it means we need to create a new root since it got split
        return 0;
    }

    RC del(const int &pageNum, IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid) {
        // if pageNum page is a leaf,
            // scan entries for matching (key, RID) -> if not found return -1
            // shift entries left, decrement numkeys and spaceoffset, write page
            // if numkeys < min, signal underflow to caller
        
        // else
            // find which child covers key K, recurse into it
            // if child signaled underflow
                // distribute with left or right sibling
                // else
                    // merge with sibling and remove into separate key from this node
                    // if removing key makes node drop below win, signal underflow upwards
        return 0;
    }

    RC
    IndexManager::deleteEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid) {
        // check if tree is empty, if so don't delete
        
        // traverse from root to find the leaf node
        // delete the entry
        // OPTIONAL: rebalance tree? lowkey might do this i feel like it'll be easy and i'm bored
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