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

PAge 0 is our hidden metadata page. WE store the root number, read, write, and append counters on it. We use this so that the data can persist across different open and closes.

Structure of the index

Keys are X bytes bc int, real, varchar types are diff sizes

Page Metadata: 
4 bytes for leaf or middle node flag | 4 bytes for number of keys | 4 bytes for free space offset | LEAF ONLY: pointer to next leaf page (unused for internal)
We need the free space offset only for varchar because the varchars can be diff lengths, but int and reals
will always be the same

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

  X = size of the key data type in bytes

Actual Data for Intermediatary Nodes:
4 bytes for the first pointer | X bytes for the key | 4 bytes for the next pointer | X bytes for the next key | ...
We need a pointer before and not only just after since everything less than the key will be at the first pointer
everything greater will be at the next


- Show your leaf-page (leaf node) design.

X = size of the key data type in bytes

Actual Data for Leaf Nodes:
X bytes for the key | RID ( 4 bytes for page number, 2 bytes for slot number)

### 5. Describe the following operation logic.
- Split
When we insert onto a full leaf, we split it by copying all the entries into a buffer. We then insert the new key into its correct spot and then divide the entries in half. We write the first half of entries back to the first original page we grabbed evertthing from. Then we write the second half to a new page that we append after we are done. We make sure to change the leaves accordingly, new page gets the old pages next leaf, and the old page gets new page as its next leaf. We make sure to change all the metadata for both pages accordingly. Then we propogate the separator back and its page, which is the smallest key of the new page we just created.

Internal splits are similar instead of copying the smallest key of the new page, it propogates that key up. We do the same thing by putting the first half on the original page, and then the rest of the keys go on the new page. The splits can propogate up and we make sure to check that if it reaches the root we split the root accordingly as well.


- Rotation

We implemented rotation separately for leaf rotation and internal rotation. When a child underflows, `redistributeOrMergeEntries` will pick a sibling, preferably the left and identify the parent's separator key `sepKeyIdx` sitting between the child and that sibling. If the sibling has extra payload, then rotation is needed.

**Leaf Rotation:** slides one data entry from the sibling into the underflowing leaf, and fixes the parent separator. If borrowing from the left sibling, it takes the sibling's last entry and prepends it to the front of the child (it used to be the largest key on the left, but is now the smallest key on the right). If borrowing from the right sibling, it takes the sibling's first entry and appends it to the end of the child.

After the move, the separator key is updated since the boundary between the two leaves has shifted. The separator is set to the smallest key now on the right-hand page (only COPY key, not the RID, since separators don't carry RIDs). It shifts the rest of the parent over to fit the new separator and adjusts the parent's free-space offset.

**Internal Rotation:** because internal keys are routing separators, internal rotation will be a 30way rotation through the parent. Nothing is copied and all the keys will physically cycle from parent -> child, sibling -> parent. If borrowing from the left sibling, the parent's separator key moves down and becomes the first key in the child. The sibling's last child-pointer will then move over to become the child's first pointer. The sibling's last key moves up and replace's the parent's separator. The sibling then drops its last (key + pointer). If borrowing from the right sibling, the parent's separator key moves down to the end of the child. THe sibling's first pointer moves to the end of the child. The sibling's first key then moves up to replace the parent's separator. The sibling drops its first (pointer + key).

Overall, an internal node will always have exactly one more pointer than keys.

- Merge/non-lazy deletion

We implemented a full non-lazy deletion with both redistribution and merging of nodes. `deleteEntry` calls a recursive helper `del` which descends from the root and removes the matching `(key, RID)` pair from the lead. If a page drops below half-full (freeSpaceOffset - METADATA_SIZE < MIN_DATA_BYTES), it propagates an "underflow" signal up through an out-parameter (single `bool&`). If a child reports underflow, the parent will call `redistributeOrMergeEntries`, which does the following:
  1. Reads the underflowing child and picks a sibling (preferably the left, if child is leftmost, than choose right). `sepKeyIdx` is the parent separator key between them.
  2. It then tries to redistribute. If the sibling has more than `MIN_DATA_BYTES` of payload, it will move one entry from the sibling to the child. If the child is a leaf, it will borrow the last entry if left sibling and first entry if right sibling. It will then update the parent separator to be the smallest key on the right of the page (only key, does not update RID). If the child is an internal node, it with rotate through the parent. The parent separator will get moved down as a key in the child. The sibling's edge pointer will move into the child, and the sibling's edge key will move up as the new parent separator.
  3. If the sibling has less than `MIN_DATA_BYTES`, it will merge. If the child is a leaf, it will append the right page's entries to the end of the left page and set `left.nextLeaf` to `right.nextLeaf` to keep the leaf chain valid. The right page is then orphaned with no free-page list. We then remove the parent's separator key and the right child's pointer. If the child is an internal node, we pull the parent's separator down as a key between the left and right's data. We then append all of right's data. The same parent cleanup as the leaf case will occur.
  4. Lastly, we cascade the changes upward. After `redistributeOrMergeEntries` returns, `del` will re-read the parent's metadata. If a merge dropped the parent below `MIN_DATA_BYTES`, the parent will signal underflow to its own parent, propagating it up the tree.
  5. If the root is an internal node with 0 keys (occurs on merge cascade), promote its lone remaining child to be the new root. The tree will thus shrink by one level.


- Duplicate key span in a page

Duplicate keys are stored based on insertion order, but they just follow one after another. Each duplicate entry should have a unique (key, RID) because they are distinct. When trying to delete, it makes sure to properly match on the key and RID. The scan will look at all the entries with that key. 


- Duplicate key span multiple pages (if applicable)
Whne there are too many duplicates, the rest will persist onto the next page. If there;s a split they'll still be next to each other since they are contigious. When scanning our functions land on the first leaf that can hold the key to make sure that we get all entries. 


### 6. Implementation Detail
- Have you added your own module or source file (.cc or .h)? 
  Clearly list the changes on files and CMakeLists.txt, if any.
  N/A



- Other implementation details:



### 7. Member contribution (for team of two)
- Explain how you distribute the workload in team.
  Lawrence Zhou: insertEntry logic, debugging, index structure
  Angelina Wang: deleteEntry, printTree, scan, file handling



### 8. Other (optional)
- Freely use this section to tell us about things that are related to the project 3, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
