        #include "src/include/ix.h"

        namespace PeterDB {
            IndexManager &IndexManager::instance() {
                static IndexManager _index_manager = IndexManager();
                return _index_manager;
            }

            int keyLenAt(const Attribute &attribute, const char *p);
            int compareKeys(const Attribute &attribute, const void *k1, const void *k2);
            int internalChildPtrOffsetAt(const Attribute &attribute, const char *page, int idx);
            int internalKeyOffsetAt(const Attribute &attribute, const char *page, int idx);
            RC leafEntrySize(const Attribute &attribute, const char *entry);

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
                        cmp = memcmp((char*)key + 4, p + 4, minLen);

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
                char buffer[PAGE_SIZE * 2] = {0};
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
                int newPageFreeSpaceOffset = METADATA_SIZE + bytesRight;
                int oldPageFreeSpaceOffset = METADATA_SIZE + bytesLeft;

                memcpy(&next, page + 12, 4);
                memcpy(newPage, &node, 4);
                memcpy(newPage + 4, &numRight, 4);
                memcpy(newPage + 8, &newPageFreeSpaceOffset, 4);
                memcpy(newPage + 12, &next, 4);
                
                ixFileHandle.appendPage(newPage);

                next = ixFileHandle.getNumberOfPages() - 1;
                newChildPage = next;

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
                    int leftFirst, rightFirst;
                    memcpy(&leftFirst, page + METADATA_SIZE, 4);
                    memcpy(&rightFirst, newPage + METADATA_SIZE, 4);
                }
                else {
                    newChildKey = new char[KEY_SIZE];
                    memcpy(newChildKey, splitptr, KEY_SIZE);
                }
                return 0;
            }

            RC insertWithSpaceInternal(IXFileHandle &ixFileHandle, const Attribute &attribute, const PageNum &pageNum, const void *key, PageNum childPage, char (&page)[PAGE_SIZE], int numKeys, int freeSpaceOffset) {
                int keyLen = keyLenAt(attribute, (char*)key);
                int entrySize = keyLen + PTR_SIZE;

                // find insert position by walking keys
                char *p = page + METADATA_SIZE + PTR_SIZE;  // skip first ptr to get to first key
                char *insertptr = page + freeSpaceOffset;
                for (int i = 0; i < numKeys; i++) {
                    if (compareKeys(attribute, key, p) < 0) {
                        insertptr = p;
                        break;
                    }
                    p += keyLenAt(attribute, p) + PTR_SIZE;
                }

                // shift everything right to make space
                char *dataEnd = page + freeSpaceOffset;
                int bytes = dataEnd - insertptr;
                memmove(insertptr + entrySize, insertptr, bytes);

                // write key then child pointer after it
                memcpy(insertptr, key, keyLen);
                memcpy(insertptr + keyLen, &childPage, PTR_SIZE);

                // update metadata
                numKeys += 1;
                freeSpaceOffset += entrySize;
                memcpy(page + 4, &numKeys, 4);
                memcpy(page + 8, &freeSpaceOffset, 4);
                ixFileHandle.writePage(pageNum, page);
                return 0;
            }

            RC insertSplitInternal(IXFileHandle &ixFileHandle, const Attribute &attribute, const PageNum &pageNum, const void *key, PageNum childPage, char (&page)[PAGE_SIZE], int numKeys, int freeSpaceOffset, void *&newChildKey, PageNum &newChildPage) {
                // almost the same as leaf
                char buffer[PAGE_SIZE * 2] = {0};
                memcpy(buffer, page + METADATA_SIZE, freeSpaceOffset - METADATA_SIZE);
                
                int keyLen = keyLenAt(attribute, (char*)key);
                int entrySize = keyLen + PTR_SIZE;


                // same search as before
                char *p = buffer + PTR_SIZE;  // skip first ptr
                char *insertptr = buffer + (freeSpaceOffset - METADATA_SIZE);  
                for (int i = 0; i < numKeys; i++) {
                    if (compareKeys(attribute, key, p) < 0) {
                        insertptr = p;
                        break;
                    }
                    p += keyLenAt(attribute, p) + PTR_SIZE;
                }

                char *dataEnd = buffer + (freeSpaceOffset - METADATA_SIZE);
                int bytes = dataEnd - insertptr;
                memmove(insertptr + entrySize, insertptr, bytes);
                memcpy(insertptr, key, keyLen);
                memcpy(insertptr + keyLen, &childPage, PTR_SIZE);

                int numEntries = numKeys + 1;
                int numLeft = numEntries / 2;
                int numRight = numEntries - numLeft - 1;  // -1 because middle key gets pushed up
                char newPage[PAGE_SIZE] = {0};

                char *splitptr = buffer + PTR_SIZE;  
                for (int i = 0; i < numLeft; i++) {
                    splitptr += keyLenAt(attribute, splitptr) + PTR_SIZE;
                }

                int middleKeyLen = keyLenAt(attribute, splitptr);
                char *rightStart = splitptr + middleKeyLen;  

                int bytesLeft = splitptr - buffer;  
                int bytesRight = (dataEnd + entrySize) - rightStart;  

                memset(page + METADATA_SIZE, 0, PAGE_SIZE - METADATA_SIZE);
                memcpy(page + METADATA_SIZE, buffer, bytesLeft);
                *(int*)(page + 4) = numLeft;
                *(int*)(page + 8) = METADATA_SIZE + bytesLeft;

                *(int*)(newPage + 0) = INTERNAL_NODE;
                *(int*)(newPage + 4) = numRight;
                *(int*)(newPage + 8) = METADATA_SIZE + bytesRight;
                *(int*)(newPage + 12) = 0;
                memcpy(newPage + METADATA_SIZE, rightStart, bytesRight);

                ixFileHandle.appendPage(newPage);
                newChildPage = ixFileHandle.getNumberOfPages() - 1;
                ixFileHandle.writePage(pageNum, page);

                newChildKey = new char[middleKeyLen];
                memcpy(newChildKey, splitptr, middleKeyLen);

                return 0;
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


                int keySize; 
                if (attribute.type == TypeVarChar) {
                    memcpy(&keySize, key, 4);
                    keySize += 4;
                }
                else {
                    keySize = 4;
                }
                if (flag == LEAF_NODE) {
                    // differentiate ints and reals vs varchars to calculate space needed
                    // if there is space
                    if (keySize + freeSpaceOffset + RID_SIZE <= PAGE_SIZE) {
                        // insert the the value into the correct spot and shift everything else over
                        if (insertWithSpaceLeaf(ixFileHandle, attribute, pageNum, key, rid, page, numKeys, freeSpaceOffset) != 0) return -1;
                        return 0;
                    }
                    // if there's NO SPACE SPLIT and pass back up
                    else {
                        if (insertSplitLeaf(ixFileHandle, attribute, pageNum, key, rid, page, numKeys, freeSpaceOffset, newChildKey, newChildPage) != 0) return -1;
                        return 0;
                    }
                }
                // if the node is an internal node
                // keep going down until you find the correct leaf node to insert into
                // if newChildKey and newChildPage come back with values, you have to insert that into this internal node
                else {
                    // choose subtree
                    int childIndex = findInternalChildIds(attribute, page, numKeys, key);
                    PageNum childPage;
                    if (attribute.type == TypeVarChar) {
                        char *cur = page + METADATA_SIZE;
                        for (int i = 0; i < childIndex; i++) {
                            cur += PTR_SIZE;  // skip pointer
                            int curLen;
                            memcpy(&curLen, cur, 4);
                            cur += 4 + curLen;  // skip key
                        }
                        memcpy(&childPage, cur, PTR_SIZE);
                    }
                    else {
                        memcpy(&childPage, page + METADATA_SIZE + childIndex * (PTR_SIZE + KEY_SIZE), PTR_SIZE);
                    }
                    insert(ixFileHandle, childPage, attribute, key, rid, newChildKey, newChildPage);

                    if (!newChildKey) return 0;
                    // we split something in child we must insert newchildentry somewhere here
                    ixFileHandle.readPage(pageNum, page);
                    memcpy(&numKeys, page + 4, 4);
                    memcpy(&freeSpaceOffset, page + 8, 4);
                    if (keySize + freeSpaceOffset + PTR_SIZE <= PAGE_SIZE) {
                        // insert the the value into the correct spot and shift everything else over
                        if (insertWithSpaceInternal(ixFileHandle, attribute, pageNum, newChildKey, newChildPage, page, numKeys, freeSpaceOffset) != 0) return -1;
                        newChildKey = nullptr;
                        return 0;
                    }
                    if (insertSplitInternal(ixFileHandle, attribute, pageNum, newChildKey, newChildPage, page, numKeys, freeSpaceOffset, newChildKey, newChildPage) !=0) return -1;            // check childEntry in case of split below

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
                    ixFileHandle.ixAppendPageCounter++;
                }
                // traverse from the root to the leaf where we can insert
                void *newChildKey = nullptr;
                PageNum newChildPage = 0;
                // insert will recursively go down until it finds a spot to place, if there is a split it handles it
                insert(ixFileHandle, ixFileHandle.rootPageNum, attribute, key, rid, newChildKey, newChildPage);

                // if newChildKey and newChildPage come back with values it means we need to create a new root since it got split
                if (newChildKey != nullptr) {
                    char newRoot[PAGE_SIZE] = {0};
                    int newKeyLen = keyLenAt(attribute, (char*)newChildKey);
                    *(int*)(newRoot + 0) = INTERNAL_NODE;
                    *(int*)(newRoot + 4) = 1;
                    *(int*)(newRoot + 8) = METADATA_SIZE + PTR_SIZE + newKeyLen + PTR_SIZE;
                    *(int*)(newRoot + 12) = 0;
                    memcpy(newRoot + METADATA_SIZE, &ixFileHandle.rootPageNum, PTR_SIZE);
                    memcpy(newRoot + METADATA_SIZE + PTR_SIZE, newChildKey, newKeyLen);
                    memcpy(newRoot + METADATA_SIZE + PTR_SIZE + newKeyLen, &newChildPage, PTR_SIZE);
                    
                    ixFileHandle.appendPage(newRoot);
                    ixFileHandle.rootPageNum = ixFileHandle.getNumberOfPages() - 1;
                    delete[] (char*)newChildKey;
                }
                return 0;
            }

            // forward declaration (defined below)
            RC leafEntrySize(const Attribute &attribute, const char *entry);

            // 0-indexed, get byte offset of idx-th child pointer in an internal node
            int internalChildPtrOffsetAt(const Attribute &attribute, const char *page, int idx) {
                int offset = METADATA_SIZE;

                for (int i = 0; i < idx; i++) {
                    offset += 4;  // skip ptr at i

                    if (attribute.type == TypeVarChar) {
                        int len;
                        memcpy(&len, page + offset, 4);
                        offset += 4 + len;
                    } else {
                        offset += 4;
                    }
                }
                return offset;
            }

            // 1-indexed, get byte offset of idx-th separator key in an internal node
            int internalKeyOffsetAt(const Attribute &attribute, const char *page, int idx) {
                int offset = METADATA_SIZE + 4;  // skip ptr 0, now at key 1

                for (int i = 1; i < idx; i++) {
                    int len = (attribute.type == TypeVarChar) ? (*(int*)(page + offset) + 4) : 4;
                    offset += len + 4;  // skip key and ptr at i
                }
                return offset;
            }

            // total byte length of a key at the given offset
            int keyLenAt(const Attribute &attribute, const char *p) {
                if (attribute.type == TypeVarChar) {
                    int len; memcpy(&len, p, 4); return 4 + len;
                }
                return KEY_SIZE;
            }

            RC redistributeOrMergeEntries(IXFileHandle &ixFileHandle, const Attribute &attribute, char *parentPage, PageNum parentPageNum,
                                    int parentNumKeys, int parentFreeSpaceOffset, int childIdx) {
                
                // read underflowing child node
                int childPtrOffset = internalChildPtrOffsetAt(attribute, parentPage, childIdx);
                PageNum childPageNum;
                memcpy(&childPageNum, parentPage + childPtrOffset, 4);

                char child[PAGE_SIZE];
                if (ixFileHandle.readPage(childPageNum, child) != 0) return -1;

                int childFlag, childNumKeys, childFSO, childNext;
                memcpy(&childFlag, child, 4);
                memcpy(&childNumKeys, child + 4, 4);
                memcpy(&childFSO, child + 8, 4);
                memcpy(&childNext, child + 12, 4);

                // pick a sibling, prefer left
                bool useLeft = (childIdx > 0);
                int sibIdx = useLeft ? (childIdx - 1) : (childIdx + 1);
                int sepKeyIdx = useLeft ? childIdx : (childIdx + 1); // parent separator key INDEX (1-based) sitting between child[sibIdx] and child[sibIdx+1]
                
                int sibPtrOffset = internalChildPtrOffsetAt(attribute, parentPage, sibIdx);
                PageNum sibPageNum;
                memcpy(&sibPageNum, parentPage + sibPtrOffset, 4);

                char sibling[PAGE_SIZE];
                if (ixFileHandle.readPage(sibPageNum, sibling) != 0) return -1;

                int sibNumKeys, sibFSO, sibNext;
                memcpy(&sibNumKeys, sibling + 4, 4);
                memcpy(&sibFSO, sibling + 8, 4);
                memcpy(&sibNext, sibling + 12, 4);

                int sibUsedBytes = sibFSO - METADATA_SIZE;

                // leaf children
                if (childFlag == LEAF_NODE) {
                    if (sibUsedBytes > MIN_DATA_BYTES) {
                        // redistribute leaves
                        char *sibData = sibling + METADATA_SIZE;
                        char *childData = child + METADATA_SIZE;

                        if (useLeft) {
                            // move sibling's last entry to front of child
                            char *p = sibData;
                            char *lastEntry = nullptr;
                            int lastSize = 0;
                            for (int i = 0; i < sibNumKeys; i++) {
                                lastEntry = p;
                                lastSize = leafEntrySize(attribute, p);
                                p += lastSize;
                            }

                            memmove(childData + lastSize, childData, childFSO - METADATA_SIZE);
                            memcpy(childData, lastEntry, lastSize);
                            sibNumKeys -= 1;  sibFSO -= lastSize;
                            childNumKeys += 1; childFSO += lastSize;
                        } else {
                            // move sibling's first entry to end of child
                            int firstSize = leafEntrySize(attribute, sibData);
                            memcpy(child + childFSO, sibData, firstSize);
                            memmove(sibData, sibData + firstSize, sibFSO - METADATA_SIZE - firstSize);

                            sibNumKeys -= 1;
                            sibFSO -= firstSize;
                            childNumKeys += 1;
                            childFSO += firstSize;
                        }

                        // update parent separator, which is smallest key on the right page (only the key)
                        char *rightData = useLeft ? child + METADATA_SIZE : sibling + METADATA_SIZE;
                        int newSepLen = keyLenAt(attribute, rightData);
                        int oldSepOffset = internalKeyOffsetAt(attribute, parentPage, sepKeyIdx);
                        int oldSepLen = keyLenAt(attribute, parentPage + oldSepOffset);

                        int tailStart = oldSepOffset + oldSepLen;
                        int tailBytes = parentFreeSpaceOffset - tailStart;
                        memmove(parentPage + oldSepOffset + newSepLen, parentPage + tailStart, tailBytes);
                        memcpy(parentPage + oldSepOffset, rightData, newSepLen);

                        parentFreeSpaceOffset += (newSepLen - oldSepLen);
                        memcpy(parentPage + 8, &parentFreeSpaceOffset, 4);

                        memcpy(child + 4, &childNumKeys, 4);
                        memcpy(child + 8, &childFSO, 4);
                        memcpy(sibling + 4, &sibNumKeys, 4);
                        memcpy(sibling + 8, &sibFSO, 4);

                        if (ixFileHandle.writePage(childPageNum, child) != 0) return -1;
                        if (ixFileHandle.writePage(sibPageNum, sibling) != 0) return -1;
                        if (ixFileHandle.writePage(parentPageNum, parentPage) != 0) return -1;

                        return 0;
                    }

                    // MERGEEE leaf babies
                    char *leftPage, *rightPage;
                    PageNum leftPageNum, rightPageNum;
                    int leftFSO, leftNumKeys, rightFSO, rightNumKeys, rightNext;
                    if (useLeft) {
                        leftPage = sibling; leftPageNum = sibPageNum;
                        rightPage = child; rightPageNum = childPageNum;
                        leftFSO = sibFSO; leftNumKeys = sibNumKeys;
                        rightFSO = childFSO; rightNumKeys = childNumKeys; rightNext = childNext;
                    } else {
                        leftPage = child; leftPageNum = childPageNum;
                        rightPage = sibling; rightPageNum = sibPageNum;
                        leftFSO = childFSO; leftNumKeys = childNumKeys;
                        rightFSO = sibFSO; rightNumKeys = sibNumKeys; rightNext = sibNext;
                    }

                    int rightDataBytes = rightFSO - METADATA_SIZE;
                    memcpy(leftPage + leftFSO, rightPage + METADATA_SIZE, rightDataBytes);
                    leftNumKeys += rightNumKeys;
                    leftFSO += rightDataBytes;

                    memcpy(leftPage + 4, &leftNumKeys, 4);
                    memcpy(leftPage + 8, &leftFSO, 4);
                    memcpy(leftPage + 12, &rightNext, 4);
                    if (ixFileHandle.writePage(leftPageNum, leftPage) != 0) return -1;

                    // remove separator + right child pointer from parent
                    int sepOffset = internalKeyOffsetAt(attribute, parentPage, sepKeyIdx);
                    int sepLen = keyLenAt(attribute, parentPage + sepOffset);
                    int removeBytes = sepLen + 4;
                    int tailStart = sepOffset + removeBytes;
                    int tailBytes = parentFreeSpaceOffset - tailStart;
                    memmove(parentPage + sepOffset, parentPage + tailStart, tailBytes);

                    parentNumKeys -= 1;
                    parentFreeSpaceOffset -= removeBytes;
                    memcpy(parentPage + 4, &parentNumKeys, 4);
                    memcpy(parentPage + 8, &parentFreeSpaceOffset, 4);
                    if (ixFileHandle.writePage(parentPageNum, parentPage) != 0) return -1;

                    return 0;
                }

                // internal children
                if (sibUsedBytes > MIN_DATA_BYTES) {
                    //redistribute internal nodes, rotate with parent
                    if (useLeft) {
                        // sibling's last (key, ptr) -> child front
                        // parent sep moves down --> sibling last key moves up
                        int sibLastKeyOffset = internalKeyOffsetAt(attribute, sibling, sibNumKeys);
                        int sibLastKeyLen = keyLenAt(attribute, sibling + sibLastKeyOffset);
                        int sibLastPtrOffset = sibLastKeyOffset + sibLastKeyLen;

                        int sepOffset = internalKeyOffsetAt(attribute, parentPage, sepKeyIdx);
                        int sepLen = keyLenAt(attribute, parentPage + sepOffset);

                        // prepend [sib_last_ptr][parent_sep] to child
                        int insertSize = 4 + sepLen;
                        int childDataLen = childFSO - METADATA_SIZE;
                        memmove(child + METADATA_SIZE + insertSize, child + METADATA_SIZE, childDataLen);
                        memcpy(child + METADATA_SIZE, sibling + sibLastPtrOffset, 4);
                        memcpy(child + METADATA_SIZE + 4, parentPage + sepOffset, sepLen);
                        childNumKeys += 1;
                        childFSO += insertSize;

                        // save the sibling's last key before we shrink sibling
                        char savedKey[PAGE_SIZE];
                        memcpy(savedKey, sibling + sibLastKeyOffset, sibLastKeyLen);

                        // sibling loses its last (key + ptr)
                        int sibRemoveBytes = sibLastKeyLen + 4;
                        sibNumKeys -= 1;
                        sibFSO -= sibRemoveBytes;

                        // replace parent separator with sibling's old last key
                        int tailStart = sepOffset + sepLen;
                        int tailBytes = parentFreeSpaceOffset - tailStart;
                        memmove(parentPage + sepOffset + sibLastKeyLen, parentPage + tailStart, tailBytes);
                        memcpy(parentPage + sepOffset, savedKey, sibLastKeyLen);
                        parentFreeSpaceOffset += (sibLastKeyLen - sepLen);
                        memcpy(parentPage + 8, &parentFreeSpaceOffset, 4);
                    } else {
                        // right sibling has spare: parent sep -> child end
                        // sibling first ptr -> child end ptr
                        // sibling first key moves up
                        int sibFirstKeyOffset = METADATA_SIZE + 4; // skip sibling's ptr 0
                        int sibFirstKeyLen = keyLenAt(attribute, sibling + sibFirstKeyOffset);

                        int sepOffset = internalKeyOffsetAt(attribute, parentPage, sepKeyIdx);
                        int sepLen = keyLenAt(attribute, parentPage + sepOffset);

                        // append [parent_sep][sibling's ptr_0] to child
                        memcpy(child + childFSO, parentPage + sepOffset, sepLen);
                        memcpy(child + childFSO + sepLen, sibling + METADATA_SIZE, 4);
                        childNumKeys += 1;
                        childFSO += sepLen + 4;

                        // save sibling's first key
                        char savedKey[PAGE_SIZE];
                        memcpy(savedKey, sibling + sibFirstKeyOffset, sibFirstKeyLen);

                        // sibling loses its first (ptr 0 + key 1)
                        int sibRemoveBytes = 4 + sibFirstKeyLen;
                        memmove(sibling + METADATA_SIZE, sibling + METADATA_SIZE + sibRemoveBytes, sibFSO - METADATA_SIZE - sibRemoveBytes);
                        sibNumKeys -= 1;
                        sibFSO -= sibRemoveBytes;

                        // replace parent sep with sibling's prev first key
                        int tailStart = sepOffset + sepLen;
                        int tailBytes = parentFreeSpaceOffset - tailStart;
                        memmove(parentPage + sepOffset + sibFirstKeyLen, parentPage + tailStart, tailBytes);
                        memcpy(parentPage + sepOffset, savedKey, sibFirstKeyLen);
                        parentFreeSpaceOffset += (sibFirstKeyLen - sepLen);
                        memcpy(parentPage + 8, &parentFreeSpaceOffset, 4);
                    }

                    memcpy(child + 4, &childNumKeys, 4);
                    memcpy(child + 8, &childFSO, 4);
                    memcpy(sibling + 4, &sibNumKeys, 4);
                    memcpy(sibling + 8, &sibFSO, 4);
                    if (ixFileHandle.writePage(childPageNum, child) != 0) return -1;
                    if (ixFileHandle.writePage(sibPageNum, sibling) != 0) return -1;
                    if (ixFileHandle.writePage(parentPageNum, parentPage) != 0) return -1;

                    return 0;
                }

                char *leftPage, *rightPage;
                PageNum leftPageNum, rightPageNum;
                int leftFSO, leftNumKeys, rightFSO, rightNumKeys;

                if (useLeft) {
                    leftPage = sibling; leftPageNum = sibPageNum;
                    rightPage = child; rightPageNum = childPageNum;
                    leftFSO = sibFSO; leftNumKeys = sibNumKeys;
                    rightFSO = childFSO; rightNumKeys = childNumKeys;
                } else {
                    leftPage = child; leftPageNum = childPageNum;
                    rightPage = sibling; rightPageNum = sibPageNum;
                    leftFSO = childFSO; leftNumKeys = childNumKeys;
                    rightFSO = sibFSO; rightNumKeys = sibNumKeys;
                }

                int sepOffset = internalKeyOffsetAt(attribute, parentPage, sepKeyIdx);
                int sepLen = keyLenAt(attribute, parentPage + sepOffset);

                // append [parent_sep][all of right's data] to left
                memcpy(leftPage + leftFSO, parentPage + sepOffset, sepLen);
                int rightDataBytes = rightFSO - METADATA_SIZE;
                memcpy(leftPage + leftFSO + sepLen, rightPage + METADATA_SIZE, rightDataBytes);

                leftNumKeys = leftNumKeys + 1 + rightNumKeys;
                leftFSO = leftFSO + sepLen + rightDataBytes;
                memcpy(leftPage + 4, &leftNumKeys, 4);
                memcpy(leftPage + 8, &leftFSO, 4);
                if (ixFileHandle.writePage(leftPageNum, leftPage) != 0) return -1;

                // remove separator + right child pointer from parent
                int removeBytes = sepLen + 4;
                int tailStart = sepOffset + removeBytes;
                int tailBytes = parentFreeSpaceOffset - tailStart;
                memmove(parentPage + sepOffset, parentPage + tailStart, tailBytes);

                parentNumKeys -= 1;
                parentFreeSpaceOffset -= removeBytes;
                memcpy(parentPage + 4, &parentNumKeys, 4);
                memcpy(parentPage + 8, &parentFreeSpaceOffset, 4);
                if (ixFileHandle.writePage(parentPageNum, parentPage) != 0) return -1;

                return 0;
            }

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
                int childPtrOffset = internalChildPtrOffsetAt(attribute, page, childIdx);
                memcpy(&childPage, page + childPtrOffset, 4);

                // RECURSIVE PART
                bool childUnderflow = false;
                if (del(ixFileHandle, childPage, attribute, key, rid, childUnderflow) != 0) return -1;
                if (!childUnderflow) return 0; // we done, no redistribution needed
                
                // if child signaled underflow
                if (redistributeOrMergeEntries(ixFileHandle, attribute, page, pageNum, numKeys, freeSpaceOffset, childIdx) != 0) return -1;
                // if internal node is now below half full, signal up
                memcpy(&numKeys, page + 4, 4);
                memcpy(&freeSpaceOffset, page + 8, 4);

                if (freeSpaceOffset - METADATA_SIZE < MIN_DATA_BYTES) underflow = true;
                            // merge with sibling and remove into separate key from this node
                            // if removing key makes node drop below win, signal underflow upwards
                return 0;
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

            // compare two keys in IX storage format
            int compareKeys(const Attribute &attribute, const void *k1, const void *k2) {
                if (attribute.type == TypeVarChar) {
                    int l1, l2;
                    memcpy(&l1, k1, 4);
                    memcpy(&l2, k2, 4);

                    int minLen = (l1 < l2) ? l1 : l2;
                    int cmp = memcmp((char*)k1 + 4, (char*)k2 + 4, minLen);

                    if (cmp != 0) return cmp < 0 ? -1 : 1;
                    if (l1 < l2) return -1;
                    if (l1 > l2) return 1;

                    return 0;
                } else if (attribute.type == TypeInt) {
                    int a, b;
                    memcpy(&a, k1, 4);
                    memcpy(&b, k2, 4);

                    if (a < b) return -1;
                    if (a > b) return 1;

                    return 0;
                } else { // TypeReal
                    float a, b;
                    memcpy(&a, k1, 4);
                    memcpy(&b, k2, 4);

                    if (a < b) return -1;
                    if (a > b) return 1;

                    return 0;
                }
            }

            // find the leaf where lower key belongs, or like leftmost leaf if lowKey is null
            PageNum findStartingLeaf(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *lowKey) {
                PageNum pageNum = ixFileHandle.rootPageNum;
                char page[PAGE_SIZE];

                while (true) {
                    if (ixFileHandle.readPage(pageNum, page) != 0) return 0;
                    int flag, numKeys;
                    memcpy(&flag, page, 4);
                    memcpy(&numKeys, page + 4, 4);
                    if (flag == LEAF_NODE) {
                        return pageNum;
                    }

                    // internal node: pick child
                    // use >= to go left on equality, so we find the first occurrence of duplicate keys
                    int childIdx;
                    if (lowKey == nullptr) {
                        childIdx = 0;
                    } else {
                        char *p = page + METADATA_SIZE + 4; // skip ptr0 to get to first key
                        childIdx = numKeys; // default: rightmost child
                        for (int i = 0; i < numKeys; i++) {
                            int keyLen = (attribute.type == TypeVarChar) ? (*(int*)p + 4) : 4;
                            int cmp;
                            if (attribute.type == TypeVarChar) {
                                int currentLen, targetLen;
                                memcpy(&currentLen, p, 4);
                                memcpy(&targetLen, lowKey, 4);
                                int minLen = (currentLen < targetLen) ? currentLen : targetLen;
                                cmp = memcmp(p + 4, (char*)lowKey + 4, minLen);
                                if (cmp == 0) {
                                    cmp = (targetLen < currentLen) ? -1 : (targetLen > currentLen ? 1 : 0);
                                }
                            } else if (attribute.type == TypeInt) {
                                int currKey = *(int*)p;
                                int newKey = *(int*)lowKey;
                                cmp = (newKey < currKey) ? -1 : (newKey > currKey ? 1 : 0);
                            } else {
                                float currKey = *(float*)p;
                                float newKey = *(float*)lowKey;
                                cmp = (newKey < currKey) ? -1 : (newKey > currKey ? 1 : 0);
                            }
                            if (cmp >= 0) { // key <= separator → go left to child i
                                childIdx = i;
                                break;
                            }
                            p += keyLen + 4;
                        }
                    }
                    int childPtrOffset = internalChildPtrOffsetAt(attribute, page, childIdx);
                    memcpy(&pageNum, page + childPtrOffset, 4);
                }
            }

            RC IndexManager::scan(IXFileHandle &ixFileHandle, const Attribute &attribute, const void *lowKey,
                                const void *highKey, bool lowKeyInclusive, bool highKeyInclusive,
                                IX_ScanIterator &ix_ScanIterator) {
                if (!ixFileHandle.isOpen) return -1;
                ix_ScanIterator.close();   // reset any prior state

                ix_ScanIterator.ixFileHandle = &ixFileHandle;
                ix_ScanIterator.attribute = attribute;
                ix_ScanIterator.lowKeyInclusive = lowKeyInclusive;
                ix_ScanIterator.highKeyInclusive = highKeyInclusive;

                // copy bounds, caller's pointers do not belong to function
                if (lowKey != nullptr) {
                    int keyLen = (attribute.type == TypeVarChar) ? (4 + *(int*)lowKey) : 4;
                    ix_ScanIterator.lowKey.assign((char*)lowKey, (char*)lowKey + keyLen);
                }
                if (highKey != nullptr) {
                    int keyLen = (attribute.type == TypeVarChar) ? (4 + *(int*)highKey) : 4;
                    ix_ScanIterator.highKey.assign((char*)highKey, (char*)highKey + keyLen);
                }

                ix_ScanIterator.isOpen = true;

                if (ixFileHandle.rootPageNum == 0) {
                    ix_ScanIterator.exhausted = true;
                    return 0;
                }

                PageNum startPage = findStartingLeaf(ixFileHandle, attribute, lowKey);
                if (startPage == 0) return -1;

                ix_ScanIterator.currentPageNum = startPage;
                if (ixFileHandle.readPage(startPage, ix_ScanIterator.currentPage) != 0) return -1;
                ix_ScanIterator.currentEntryIdx = 0;
                ix_ScanIterator.exhausted = false;

                // advance past entries < lowKey (or <= if not inclusive)
                if (lowKey != nullptr) {
                    while (true) {
                        int numKeys;
                        memcpy(&numKeys, ix_ScanIterator.currentPage + 4, 4);
                        char *p = ix_ScanIterator.currentPage + METADATA_SIZE;
                        bool found = false;
                        for (int i = 0; i < numKeys; i++) {
                            int cmp = compareKeys(attribute, p, lowKey);
                            if (cmp > 0 || (cmp == 0 && lowKeyInclusive)) {
                                ix_ScanIterator.currentEntryIdx = i;
                                found = true;
                                break;
                            }
                            p += leafEntrySize(attribute, p);
                            ix_ScanIterator.currentEntryIdx = i + 1;
                        }
                        if (found) break;
                        // all entries on this page are < lowKey, advance to next leaf
                        int nextLeaf;
                        memcpy(&nextLeaf, ix_ScanIterator.currentPage + 12, 4);
                        if (nextLeaf == 0) {
                            ix_ScanIterator.exhausted = true;
                            break;
                        }
                        ix_ScanIterator.currentPageNum = nextLeaf;
                        if (ixFileHandle.readPage(nextLeaf, ix_ScanIterator.currentPage) != 0) {
                            ix_ScanIterator.exhausted = true;
                            break;
                        }
                        ix_ScanIterator.currentEntryIdx = 0;
                    }
                }
                return 0;
            }

            RC printNode(IXFileHandle &ixFileHandle, PageNum pageNum, const Attribute &attribute, std::ostream &out) {
                char page[PAGE_SIZE];
                if (ixFileHandle.readPage(pageNum, page) != 0) return -1;

                int flag, numKeys, freeSpaceOffset;
                memcpy(&flag, page, 4);
                memcpy(&numKeys, page + 4, 4);
                memcpy(&freeSpaceOffset, page + 8, 4);

                // leaf node
                if (flag == LEAF_NODE) {
                    out << "{\"keys\":[";
                    char *p = page + METADATA_SIZE;
                    bool first = true;
                    int i = 0;
                    while (i < numKeys) {
                        // read this key
                        std::string keyStr;
                        int entrySize;
                        if (attribute.type == TypeVarChar) {
                            int len; memcpy(&len, p, 4);
                            keyStr.assign(p + 4, len);
                            entrySize = 4 + len + RID_SIZE;
                        } else if (attribute.type == TypeInt) {
                            int k; memcpy(&k, p, 4);
                            keyStr = std::to_string(k);
                            entrySize = KEY_SIZE + RID_SIZE;
                        } else {
                            float k; memcpy(&k, p, 4);
                            keyStr = std::to_string(k);
                            entrySize = KEY_SIZE + RID_SIZE;
                        }

                        if (!first) out << ",";
                        first = false;
                        out << "\"" << keyStr << ":[";

                        // collect all RIDs with the same key
                        bool firstRid = true;
                        while (i < numKeys) {
                            // check if this entry has the same key
                            std::string curKeyStr;
                            if (attribute.type == TypeVarChar) {
                                int len; memcpy(&len, p, 4);
                                curKeyStr.assign(p + 4, len);
                            } else if (attribute.type == TypeInt) {
                                int k; memcpy(&k, p, 4);
                                curKeyStr = std::to_string(k);
                            } else {
                                float k; memcpy(&k, p, 4);
                                curKeyStr = std::to_string(k);
                            }
                            if (curKeyStr != keyStr) break;

                            unsigned ridPage;
                            unsigned short ridSlot;
                            memcpy(&ridPage, p + entrySize - RID_SIZE, 4);
                            memcpy(&ridSlot, p + entrySize - RID_SIZE + 4, 2);

                            if (!firstRid) out << ",";
                            firstRid = false;
                            out << "(" << ridPage << "," << ridSlot << ")";

                            p += entrySize;
                            i++;
                        }
                        out << "]\"";
                    }
                    out << "]}";
                    return 0;
                }

                // internal node
                out << "{\"keys\":[";
                char *p = page + METADATA_SIZE + 4;  // skip ptr 0
                for (int i = 0; i < numKeys; i++) {
                    std::string keyStr;
                    int keyLen;
                    if (attribute.type == TypeVarChar) {
                        int len; memcpy(&len, p, 4);
                        keyStr.assign(p + 4, len);
                        keyLen = 4 + len;
                    } else if (attribute.type == TypeInt) {
                        int k; memcpy(&k, p, 4);
                        keyStr = std::to_string(k);
                        keyLen = KEY_SIZE;
                    } else {
                        float k; memcpy(&k, p, 4);
                        keyStr = std::to_string(k);
                        keyLen = KEY_SIZE;
                    }

                    if (i > 0) out << ",";
                    out << "\"" << keyStr << "\"";
                    p += keyLen + 4;  // skip key + next ptr
                }
                out << "],\"children\":[";

                // walk children, numKeys + 1
                for (int i = 0; i <= numKeys; i++) {
                    int childPtrOffset = internalChildPtrOffsetAt(attribute, page, i);
                    PageNum childPageNum;
                    memcpy(&childPageNum, page + childPtrOffset, 4);

                    if (i > 0) out << ",";
                    if (printNode(ixFileHandle, childPageNum, attribute, out) != 0) return -1;
                }
                out << "]}";
                return 0;
            }


            RC IndexManager::printBTree(IXFileHandle &ixFileHandle, const Attribute &attribute, std::ostream &out) const {
                if (ixFileHandle.rootPageNum == 0) {
                    out << "{\"keys\":[]}";
                    return 0;
                }

                return printNode(ixFileHandle, ixFileHandle.rootPageNum, attribute, out);
            }

            IX_ScanIterator::IX_ScanIterator() {
            }

            IX_ScanIterator::~IX_ScanIterator() {
            }

            RC IX_ScanIterator::getNextEntry(RID &rid, void *key) {
                if (!isOpen || exhausted) return IX_EOF;

                while (true) {
                    int numKeys, nextLeaf;
                    memcpy(&numKeys,  currentPage + 4,  4);
                    memcpy(&nextLeaf, currentPage + 12, 4);

                    // exhausted this leaf → move to next, or end of scan
                    if (currentEntryIdx >= numKeys) {
                        if (nextLeaf == 0) { exhausted = true; return IX_EOF; }
                        if (nextLeaf == currentPageNum) { exhausted = true; return IX_EOF; }
                        currentPageNum = nextLeaf;
                        if (ixFileHandle->readPage(currentPageNum, currentPage) != 0) {
                            exhausted = true;
                            return IX_EOF;
                        }
                        currentEntryIdx = 0;
                        continue;
                    }

                    // walk to the currentEntryIdx-th entry on this page
                    char *p = currentPage + METADATA_SIZE;
                    for (int i = 0; i < currentEntryIdx; i++) {
                        p += leafEntrySize(attribute, p);
                    }
                    int entrySize = leafEntrySize(attribute, p);

                    // check upper bound
                    if (!highKey.empty()) {
                        int cmp = compareKeys(attribute, p, highKey.data());
                        if (cmp > 0 || (cmp == 0 && !highKeyInclusive)) {
                            exhausted = true;
                            return IX_EOF;
                        }
                    }

                    // copy key out (everything except the 6-byte RID at the end)
                    int keyLen = entrySize - RID_SIZE;
                    memcpy(key, p, keyLen);

                    unsigned ridPage;
                    unsigned short ridSlot;
                    memcpy(&ridPage, p + keyLen,     4);
                    memcpy(&ridSlot, p + keyLen + 4, 2);
                    rid.pageNum = ridPage;
                    rid.slotNum = ridSlot;

                    currentEntryIdx++;
                    return 0;
                }
            }

            RC IX_ScanIterator::close() {
                isOpen = false;
                exhausted = true;
                lowKey.clear();
                highKey.clear();
                ixFileHandle = nullptr;
                currentEntryIdx = 0;
                return 0;
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