## Project 1 Report


### 1. Basic information
 - Team #: 7
 - Github Repo Link: https://github.com/angeliinawang/cs122c-spring26-angeliinawang-lawwzhou
 - Student 1 UCI NetID: angeliw9
 - Student 1 Name: Angelina Wang
 - Student 2 UCI NetID: zhoulh
 - Student 2 Name: Lawrence Zhou


### 2. Internal Record Format
- Show your record format design.

[dir 0 - dir 1 - dir 2 | field 1 - field 2 - field 3]
The beginning of the record is the directory which stores offsets to the end of each field of the actual data in the record. Each directory entry is 2 bytes since they are unsigned shorts and then the field sizes depend on the type of var and actual data size.


- Describe how you store a null field.

Previously, to store null fields, we made the offset of the current field to the be equal to the previous if it the current was null. However, this failed when it came to dealing with a VARCHAR that had an empty string since this technically was valid and not null. It failed because it had equal offsets and took up zero bytes, so instead we swtiched to using 0xFFFF for our offset in the directory if a field was null. This works because it is an invalid offset since it's equal to 65535 and page size is 4096 bytes. We definitely want to see if there is a better way than storing an invalid offset, but this approach currently works.


- Describe how you store a VarChar field.

For the varchar field we use the directory offset in order to determine the size of the text. There is no metadata in the actual field data, it's just the raw characters and you just use the offset of the current and previous to calculate the length of the characters.

- Describe how your record design satisfies O(1) field access.

To access a record i field in O(1) time. With the directory of offsets to the end of every field, you can make a jump from the directory to the desired field in O(1). ex: if you want the ith field, you just go to the ith directory with record + i * 2 because each directory is unsigned short. You can get to the directory of a record using the slot table, the slot table has offsets to the start of the directory of every record. So when you pass in an RID to read, you jump to that slot number in O(1), then you jump to the record directory in O(1), then you use that to jump to the field in O(1).


### 3. Page Format
- Show your page format design.

Page Layout:
Records .... -> Free Space <- Slot Table <- Metadata
Slot table will dynamically increase left as we add more records
Records will dynamically increase right as we go right
Our slots are in format (unsigned short offset (2 bytes), unsigned short length (2 bytes)) -> 4 bytes total
We can use shorts because the max that these numbers can be is 4096 bytes
At the end, we also have page metadata, basically 2 more unsigned shorts one to represent the free space offset
and then one to represent the number of slots in the current page.
that way to quickly see if we have enough space we compute the offset - number of slots * slot_size - page_metadata

- Explain your slot directory design if applicable.

Slot table will dynamically increase left as we add more records
Our slots are in format (unsigned short offset (2 bytes), unsigned short length (2 bytes)) -> 4 bytes total
Length will tell us how many bytes the corresponding record takes up.
Offset tells us where the start of the directory for the corresponding reocrd is.
We can use shorts because the max that these numbers can be is 4096 bytes


### 4. Page Management
- Show your algorithm of finding next available-space page when inserting a record.

First we convert our record to a byte array to understand how much space is required for this record. 
We start off by checking the last page first, this makes sense logically because the last page is msot recently used and so it is more likely to have free space there.
For all pages, we use a function to calculate the amount of free space on the page and then we compare that to how much space is required for this record. If the last page has no space, we start at the first page, and then go through each page until we reach the second to last. If none of the pages have space, we just create and append a new page. Otherwise, if any of these pages have space then we designate that as the page we'll be writing to.
checkFreeSpace(page) calculation:
(int)PAGE_SIZE - PAGE_METADATA - (int)numSlots * SLOT_SIZE - (int)freeSpaceOffset - SLOT_SIZE;
We also need to make sure not only is there enough free space but also space for a new slot to be entered.
We use ints because we were running into unsigned integer underflow.

- How many hidden pages are utilized in your design?

We use only 1 hidden page in our design, which is page 0, the first 4096 bytes of the file. Thus, all data pages start at offset `PAGE_SIZE` (4096), so data page N is stored at file offset `(N + 1) * PAGE_SIZE`.

- Show your hidden page(s) format design if applicable

The hidden page stores three `unsigned` integer counters in the first 12 bytes:

| Bytes 0–3 | Bytes 4–7 | Bytes 8–11 | Bytes 12–4095 |
|---|---|---|---|
| readPageCounter | writePageCounter | appendPageCounter | unused (zeroed) |

These counters track the total number of read, write, and append operations performed on the file over its lifetime. They are loaded into the `FileHandle` when the file is opened (`openFile`) and written back to the hidden page when the file is closed (`closeFile`), so the counter values persist across open/close cycles.

### 5. Implementation Detail
- Other implementation details goes here.

**PagedFileManager (Singleton):** The `PagedFileManager` uses the singleton pattern, where a single static instance is shared across the application. Copy construction and assignment are disabled for this class.

**File I/O:** We use the standard C `FILE*` API (`fopen`, `fread`, `fwrite`, `fseek`, `ftell`, `fflush`, `fclose`) for portability. Files are opened in binary read-write mode (`"rb+"`) to ensure no newline translation occurs, which could corrupt the raw page data. All writes are immediately flushed to disk via `fflush` to ensure durability.

**File existence check:** A helper function `fileExists()` uses `stat()` to check whether a file exists before creating, destroying, or opening it.

**Page count:** `getNumberOfPages()` seeks to the end of the file, gets the file size via `ftell()`, and computes `(fileSize / PAGE_SIZE) - 1` to exclude the hidden header page.

**Error handling:** All functions return 0 on success and -1 on failure. Defensive checks prevent creating duplicate files, opening a file into an already-in-use `FileHandle`, reading/writing beyond the current page count, and operating on a null file pointer.

**FileHandle destructor:** The `FileHandle` destructor also keeps counters and closes the file as a safety net in case `closeFile` is not called explicitly.



### 6. Member contribution (for team of two)
- Explain how you distribute the workload in team.

**Angelina Wang:** Implemented the PagedFileManager and FileHandle modules (file creation/destruction, page read/write/append, hidden page with persistent counters). Also performed debugging and testing across the project using Valgrind and GDB.

**Lawrence Zhou:** Implemented insertRecord, readRecord, and printRecord. Worked on the corresponding sections of the report.


### 7. Other (optional)
- Freely use this section to tell us about things that are related to the project 1, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
