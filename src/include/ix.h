#ifndef _ix_h_
#define _ix_h_

#include <vector>
#include <string>

#include "pfm.h"
#include "rbfm.h" // for some type declarations only, e.g., RID and Attribute

# define IX_EOF (-1)  // end of the index scan
# define METADATA_SIZE 16 // header for the index pages
# define INTERNAL_NODE 0
# define LEAF_NODE 1
# define RID_SIZE 8

/*
Structure of the index

Page Metadata: 
4 bytes for leaf or middle node flag | 4 bytes for number of keys | 4 bytes for free space offset | LEAF ONLY: pointer to next leaf page (unused for internal)
We need the free space offset only for varchar because the varchars can be diff lengths, but int and reals
will always be the same

Actual Data for Intermediatary Nodes:
4 bytes for the first pointer | X bytes for the key | 4 bytes for the next pointer | X bytes for the next key | ...
We need a pointer before and not only just after since everything less than the key will be at the first pointer
everything greater will be at the next

Actual Data for Leaf Nodes:
X bytes for the key | RID ( 4 bytes for page number, 4 bytes for slot number)
*/

namespace PeterDB {
    class IX_ScanIterator;

    class IXFileHandle;

    class IndexManager {

    public:
        static IndexManager &instance();

        // Create an index file.
        RC createFile(const std::string &fileName);

        // Delete an index file.
        RC destroyFile(const std::string &fileName);

        // Open an index and return an ixFileHandle.
        RC openFile(const std::string &fileName, IXFileHandle &ixFileHandle);

        // Close an ixFileHandle for an index.
        RC closeFile(IXFileHandle &ixFileHandle);

        // Insert an entry into the given index that is indicated by the given ixFileHandle.
        RC insertEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid);

        // Delete an entry from the given index that is indicated by the given ixFileHandle.
        RC deleteEntry(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *key, const RID &rid);

        // Initialize and IX_ScanIterator to support a range search
        RC scan(IXFileHandle &ixFileHandle,
                const Attribute &attribute,
                const void *lowKey,
                const void *highKey,
                bool lowKeyInclusive,
                bool highKeyInclusive,
                IX_ScanIterator &ix_ScanIterator);

        // Print the B+ tree in pre-order (in a JSON record format)
        RC printBTree(IXFileHandle &ixFileHandle, const Attribute &attribute, std::ostream &out) const;

    protected:
        IndexManager() = default;                                                   // Prevent construction
        ~IndexManager() = default;                                                  // Prevent unwanted destruction
        IndexManager(const IndexManager &) = default;                               // Prevent construction by copying
        IndexManager &operator=(const IndexManager &) = default;                    // Prevent assignment

    };

    class IX_ScanIterator {
    public:

        // Constructor
        IX_ScanIterator();

        // Destructor
        ~IX_ScanIterator();

        // Get next matching entry
        RC getNextEntry(RID &rid, void *key);

        // Terminate index scan
        RC close();
    };

    class IXFileHandle {
    public:

        // variables to keep counter for each operation
        unsigned ixReadPageCounter;
        unsigned ixWritePageCounter;
        unsigned ixAppendPageCounter;

        // Constructor
        IXFileHandle();

        // Destructor
        ~IXFileHandle();

        // Put the current counter values of associated PF FileHandles into variables
        RC collectCounterValues(unsigned &readPageCount, unsigned &writePageCount, unsigned &appendPageCount);

        // wrap a pfm handle
        FileHandle fileHandle;
        PageNum rootPageNum; // 0 means empty tree
        bool isOpen;

        // page I/O updates ix counters
        RC readPage(PageNum pageNum, void *data);
        RC writePage(PageNum pageNum, const void *data);
        RC appendPage(const void *data);
        unsigned getNumberOfPages();

        // manage hidden page
        RC readHiddenPage(); // load rootPageNum + counters from page 0
        RC writeHiddenPage(); // flush rootPageNum + counters to page 0

    };
}// namespace PeterDB
#endif // _ix_h_
