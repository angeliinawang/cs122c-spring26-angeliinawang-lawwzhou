## Project 2 Report


### 1. Basic information
 - Team #: 7
 - Github Repo Link: https://github.com/angeliinawang/cs122c-spring26-angeliinawang-lawwzhou
 - Student 1 UCI NetID: angeliw9
 - Student 1 Name: Angelina Wang
 - Student 2 UCI NetID: zhoulh
 - Student 2 Name: Lawrence Zhou

### 2. Meta-data
- Show your meta-data design (Tables and Columns table) and information about each column.

The catalog consists of two RBF files, `Tables` and `Columns`. They are both regular record-based files (no special header or layout). The catalog follows their name: rows that describe the schemas of `Tables` and `Columns` are inserted into `Columns` during `createCatalog`.

**Tables** schema (3 columns):

| Column | Type | Length |
|---|---|---|
| table-id | TypeInt | 4 |
| table-name | TypeVarChar | 50 |
| file-name | TypeVarChar | 50 |

**Columns** schema (5 columns):

| Column | Type | Length |
|---|---|---|
| table-id | TypeInt | 4 |
| column-name | TypeVarChar | 50 |
| column-type | TypeInt | 4 |
| column-length | TypeInt | 4 |
| column-position | TypeInt | 4 |

`table-id = 1` is used for `Tables` and `table-id = 2` is used for `Columns`. Additional user tables will be assigned ids starting with id = 3. We compute the next available id by scanning `Tables` for `max(table-id) + 1` during `createTable`. `column-position` is 1-indexed and used to recover attribute order in `getAttributes`, since the records on a page are not guaranteed to be returned by scan in insertion order after deletes or updates.


### 3. Internal Record Format (in case you have changed from P1, please re-enter here)
- Show your record format design.

[dir 0 - dir 1 - dir 2 | field 1 - field 2 - field 3]
The beginning of the record is the directory that stores offsetst to the end of each field of the actual data in the record. Each directory entry is 2 bytes long since they are unsigned shorts and then the field sizes depend on the type of var and actual data size.

- Describe how you store a null field.

To store null fields, we set the directory entry of that field to `0xFFFF` (an invalid offset, since `0xFFFF = 65535` is larger than `PAGE_SIZE = 4096`). The null field contributes zero bytes to the data section. On read, we detect `0xFFFF` and flip the corresponding bit in the output null bitmap.

- Describe how you store a VarChar field.

For the varchar field we use the directory offset to determine the size of the text. There is no metadata in the actual field data, it is just the raw characters and you just use the offset of the current and previous to calculate the length of the characters. When a previous field is null, we traverse backwards through the directory until we find a non-null entry to use as the previous offset.

- Describe how your record design satisfies O(1) field access.

You can access a record's i-th field in O(1) time. With the directory of offsets to the end of every field, you can make a jump from the directory to the desired field in O(1).
Example: if you want the i-th field, just go to the i-th directory entry with `record + i * 2` because each directory entry is an unsigned short. You can get to the directory of a record using the slot table, which has offsets to the start of the directory of every record. When you pass in an RID to read, you jump to that slot number in O(1), then to the record directory in O(1), then you use that to jump to the field in O(1).


### 4. Page Format (in case you have changed from P1, please re-enter here)
- Show your page format design.

Page Layout:
Records ... -> Free Space <- Slot Table <- Metadata
Slot table will dynamically increase left as we add more records.
Records will dynamically increase right as we go right.
Our slots are in format (unsigned short offset (2 bytes), unsigned short length (2 bytes)) -> 4 bytes total.
We can use shorts because the max that these numbers can be is 4096 bytes.
At the end, we also have page metadata, basically 2 more unsigned shorts: one to represent the free space offset and one to represent the number of slots in the current page.
That way to quickly see if we have enough space we compute the offset - number of slots * slot_size - page_metadata.

- Explain your slot directory design if applicable.

Slot table will dynamically increase left as we add more records.
Our slots are in format (unsigned short offset (2 bytes), unsigned short length (2 bytes)) -> 4 bytes total.
Length will tell us how many bytes the corresponding record takes up.
Offset tells us where the start of the directory for the corresponding record is.
A slot's offset is set to `0xFFFF` to mark a deleted slot. Slot indices never shift on delete — the slot stays at the same index but is treated as a tombstoned entry, so RIDs of other records on the page remain stable.


### 5. Page Management (in case you have changed from P1, please re-enter here)
- How many hidden pages are utilized in your design?

We use only 1 hidden page in our design, which is page 0, the first 4096 bytes of the file. Thus, all data pages start at offset `PAGE_SIZE` (4096), so data page N is stored at file offset `(N + 1) * PAGE_SIZE`.

- Show your hidden page(s) format design if applicable

The hidden page stores three `unsigned` integer counters in the first 12 bytes:

| Bytes 0–3 | Bytes 4–7 | Bytes 8–11 | Bytes 12–4095 |
|---|---|---|---|
| readPageCounter | writePageCounter | appendPageCounter | unused (zeroed) |

These counters track the total number of read, write, and append operations performed on the file over its lifetime. They are loaded into the `FileHandle` when the file is opened (`openFile`) and written back to the hidden page when the file is closed (`closeFile`), so the counter values persist across open/close cycles.


### 6. Describe the following operation logic.
- Delete a record

We read the page that contains the RID and look at the slot's offset. If the offset is already `0xFFFF`, the slot was already deleted and we return -1. Otherwise we mark the slot's offset as `0xFFFF` to mark it as deleted. We then call `compactPage`, to shift all records which are currently to the right of the deleted record to the left by `dataLen` bytes with the `memmove` method. During compaction, we walk the slot table and decrement the offset of every non-deleted slot whose offset was greater than the deleted record's start, and we decrement `freeSpaceOffset` by `dataLen`. Finally we write the updated page back. The slot index itself is preserved so RIDs of other records are stable.

- Update a record

We read the page and convert the new data to a byte array to find its size. There are three different cases:

  1. **New record fits in old space (`dataLen >= outputSize`):** We overwrite the old record in place at the same offset, then call `compactPage` for the extra `dataLen - outputSize` bytes, shifting the rest of the page left. The slot's length is then updated to `outputSize`.
  2. **New record is larger but still fits on the page:** We compact the old record away entirely, then write the new record at the current `freeSpaceOffset`. The slot is updated with the new offset and length, and `freeSpaceOffset` is increased by `outputSize`.
  3. **New record is larger and doesn't fit:** We call `insertRecord` to place the new record on a different page (returns a new RID) and write a 9-byte tombstone (1 byte `TOMBSTONE_FLAG = 0x01`, 4 bytes pageNum, 4 bytes slotNum) at the original offset. If the tombstone is smaller than the old record, we compact the difference. The slot's length is set to the tombstone length (9). To make sure even very small records can later become tombstones, `insertRecord` allocates at least 9 bytes per record (`max(outputSize, 9)`).

When `readRecord` and `readAttribute` look up a record, they first check the byte at the slot's offset. If it is `TOMBSTONE_FLAG`, the record has been moved, so they read the new RID stored there and call themselves again on that RID.

- Scan on normal records

`scan` initializes an `RBFM_ScanIterator` with the file handle, record descriptor, condition attribute, comparison operator, comparison value, projected attribute names, and sets `(currentPage, currentSlot)` to (0, 0). `getNextRecord` walks page-by-page, slot-by-slot, and reads the offset for each slot. if the record passes the filter (or the op is set to `NO_OP`), it builds the projected output by walking the directory in the same pattern as `byteArrayToData`, producing a record whose layout is `[null bitmap][projected fields...]` with the null bitmap sized for the projected attribute count. The iterator advances `currentSlot` after returning, and bumps to the next page when it reaches the page's slot count. Scan returns `RBFM_EOF` when no more pages remain.

- Scan on deleted records

When a slot's offset is `0xFFFF`, the iterator skips it (using `currentSlot++` and `continue`) without reading any data.

- Scan on updated records

If a slot points at a record whose first byte is `TOMBSTONE_FLAG`, the iterator will skip it. The forwarded record will be returned later in the scan when the iterator reaches the new page/slot, so we don't follow tombstones during scan to avoid duplicate results.


### 7. Implementation Detail
- Other implementation details goes here.

**Catalog write-protection:** `createTable`, `deleteTable`, `insertTuple`, `deleteTuple` and `updateTuple` automatically reject the names "Tables" and "Columns" up front. The RM still uses the catalog internally through reads (`getAttributes` and `scan`), so the rule is that no user can write to the catalog.

**Rollback in `createCatalog`::** We create `Tables` first and then `Columns`. Thus, if the `Columns` fails to be created, we will destroy the `Tables` file before turning -1 so that it does not leave a half-built, incomplete catalog on the disk.

**`deleteCatalog` will discover user files by scanning:** Since user files are not tracked anywhere outside the catalog itself, `deleteCatalog` scans `Tables` and projects `file-name` to gather every file the user created. It then destroys each one and subsequently `Tables` and `Columns`. The two last destroys help when the catalog is partially built.

**`deleteTable` also cleans up `Columns`:** `Tables` is scanned by name to find the row and its `table-id`, then `Columns` is scanned for every row with that id. We collect `Columns` RIDs into a vector before deleting any of them, since deleting from a file while scanning can lead to invalidating the iterator. After both deletes, we destroy the actual user file.

**`getAttributes` uses two scans:** The first scan goes through `Tables` and filters it by name to recover each `table-id`. The second scan goes through `Columns`, applying the `table-id` filter, collecting (column-positionm Attribute) pairs. We then sort by `column-position` at the end before returning the schema.


### 8. Member contribution (for team of two)
- Explain how you distribute the workload in team.

**Angelina Wang:**: Finished Catalog functions in RM

**Lawrence Zhou:**: Finished RBFM, updateRecord, Scan, ScanIterator, getNextRecord

**Both**: Finished wrapper functions for allowing RM to use RBFM functions for all the tuple methods


### 9. Other (optional)
- Freely use this section to tell us about things that are related to the project 1, but not related to the other sections (optional)

We extended `insertRecord` to allocate at least 9 bytes per record (`max(outputSize, 9)`) so that any record can later be replaced in-place by a 9-byte tombstone during `updateRecord`. Without this, very small records on a tightly-packed page could not be tombstoned without compaction.

- Feedback on the project to help improve the project. (optional)
