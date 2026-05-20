## Project 3 Report


### 1. Basic information
 - Team #: 7
 - Github Repo Link: https://github.com/angeliinawang/cs122c-spring26-angeliinawang-lawwzhou
 - Student 1 UCI NetID: angeliw9
 - Student 1 Name: Angelina Wang
 - Student 2 UCI NetID: zhoulh
 - Student 2 Name: Lawrence Zhou


### 2. Meta-data page in an index file
- Show your meta-data page of an index design if you have any. 



### 3. Index Entry Format
- Show your index entry design (structure). 

  - entries on internal nodes: 

    We store internal pages using an alternating sequence of child pointers and separator keys, with one more pointer than keys:

    `[ptr_0 (4 bytes)] [key_1] [ptr_1 (4 bytes)] [key_2] ... [key_N] [ptr_N (4 bytes)]`

    Each separator key occupies:
      - `TypeInt/TypeReal`: 4 bytes
      - `TypeChar:` `[4-byte length][length bytes of chars]`

    Separator keys will hold no RID as they only exist to route searches to the correct subtree.
  
  - entries on leaf nodes:
    
    Leaf pages store data entries as `[key][6-byte RID]` packed contiguously and are sorted by key (or RID, in the case of duplicates):
      - `TypeInt/TypeReal`: 4-byte key + 6-byte RID = 10 bytes per entry
      - `TypeChar`: `[4-byte length][N chars]` + 6-byte RID = 10 + N bytes per entry

    The RID is encoded as a 4-byte `pageNum` followed by a 2-byte `slotNum`, thus making `RID_SIZE` 6 bytes total.



### 4. Page Format
- Show your internal-page (non-leaf node) design.



- Show your leaf-page (leaf node) design.



### 5. Describe the following operation logic.
- Split



- Rotation


- Merge/non-lazy deletion

We implemented a full non-lazy deletion with both redistribution and merging of nodes. `deleteEntry` calls a recursive helper `del` which descends from the root and removes the matching `(key, RID)` pair from the lead. If a page drops below half-full (freeSpaceOffset - METADATA_SIZE < MIN_DATA_BYTES), it propagates an "underflow" signal up through an out-parameter (single `bool&`). If a child reports underflow, the parent will call `redistributeOrMergeEntries`, which does the following:
  1. Reads the underflowing child and picks a sibling (preferably the left, if child is leftmost, than choose right). `sepKeyIdx` is the parent separator key between them.
  2. It then tries to redistribute. If the sibling has more than `MIN_DATA_BYTES` of payload, it will move one entry from the sibling to the child. If the child is a leaf, it will borrow the last entry if left sibling and first entry if right sibling. It will then update the parent separator to be the smallest key on the right of the page (only key, does not update RID). If the child is an internal node, it with rotate through the parent. The parent separator will get moved down as a key in the child. The sibling's edge pointer will move into the child, and the sibling's edge key will move up as the new parent separator.
  3. If the sibling has less than `MIN_DATA_BYTES`, it will merge. If the child is a leaf, it will append the right page's entries to the end of the left page and set `left.nextLeaf` to `right.nextLeaf` to keep the leaf chain valid. The right page is then orphaned with no free-page list. We then remove the parent's separator key and the right child's pointer. If the child is an internal node, we pull the parent's separator down as a key between the left and right's data. We then append all of right's data. The same parent cleanup as the leaf case will occur.
  4. Lastly, we cascade the changes upward. After `redistributeOrMergeEntries` returns, `del` will re-read the parent's metadata. If a merge dropped the parent below `MIN_DATA_BYTES`, the parent will signal underflow to its own parent, propagating it up the tree.
  5. If the root is an internal node with 0 keys (occurs on merge cascade), promote its lone remaining child to be the new root. The tree will thus shrink by one level.


- Duplicate key span in a page



- Duplicate key span multiple pages (if applicable)



### 6. Implementation Detail
- Have you added your own module or source file (.cc or .h)? 
  Clearly list the changes on files and CMakeLists.txt, if any.



- Other implementation details:



### 7. Member contribution (for team of two)
- Explain how you distribute the workload in team.



### 8. Other (optional)
- Freely use this section to tell us about things that are related to the project 3, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
