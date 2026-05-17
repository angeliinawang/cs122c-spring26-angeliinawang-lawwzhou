#include "src/include/rm.h"
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace PeterDB {
    RelationManager &RelationManager::instance() {
        static RelationManager _relation_manager = RelationManager();
        return _relation_manager;
    }

    RelationManager::RelationManager() = default;

    RelationManager::~RelationManager() = default;

    RelationManager::RelationManager(const RelationManager &) = default;

    RelationManager &RelationManager::operator=(const RelationManager &) = default;

    RC RelationManager::createCatalog() {
        // check whether catalog exists already, also create two files
        auto &rbfm = RecordBasedFileManager::instance();
        if (rbfm.createFile("Tables") != 0) return -1;
        if (rbfm.createFile("Columns") != 0) {
            rbfm.destroyFile("Tables"); // roll back
            return -1;
        }

        // get schemas
        auto tablesSchema = getTablesSchema();
        auto columnsSchema = getColumnsSchema();

        // open Tables file
        FileHandle tFH, cFH;
        if (rbfm.openFile("Tables", tFH) != 0) return -1;
        if (rbfm.openFile("Columns", cFH) != 0) return -1;

        RID rid;
        char buf[PAGE_SIZE];

        // insert row describing Tables table itself
        serializeTablesRow(buf, 1, "Tables", "Tables");
        rbfm.insertRecord(tFH, tablesSchema, buf, rid);
        // insert row describing Columns table itself
        serializeTablesRow(buf, 2, "Columns", "Columns");
        rbfm.insertRecord(tFH, tablesSchema, buf, rid);

        // insert rows describing every column of Tables table
        for (int i = 0; i < tablesSchema.size(); i++) {
            const auto &a = tablesSchema[i];
            serializeColumnsRow(buf, 1, a.name, a.type, a.length, i+1);
            rbfm.insertRecord(cFH, columnsSchema, buf, rid);
        }
        // insert rows describing every column of Columns table
        for (int i = 0; i < columnsSchema.size(); i++) {
            const auto &a = columnsSchema[i];
            serializeColumnsRow(buf, 2, a.name, a.type, a.length, i+1);
            rbfm.insertRecord(cFH, columnsSchema, buf, rid);
        }
        
        rbfm.closeFile(tFH);
        rbfm.closeFile(cFH);
        
        return 0;
    }

    RC RelationManager::deleteCatalog() {
        auto &rbfm = RecordBasedFileManager::instance();
        
        FileHandle tFH;
        if (rbfm.openFile("Tables", tFH) != 0) return -1;

        auto tableSchema = getTablesSchema();

        RBFM_ScanIterator it;
        std::vector<std::string> projection = {"file-name"};
        if (rbfm.scan(tFH, tableSchema, "", NO_OP, nullptr, projection, it) != 0) {
            rbfm.closeFile(tFH);
            return -1;
        }

        // scan returns row format
        // [1 byte null bitmap][4 byte length][chars...]
        std::vector<std::string> filesToDelete;
        RID rid;
        char row[PAGE_SIZE];

        // delete eachh file
        while (it.getNextRecord(rid, row) != RBFM_EOF) {
            if (row[0] & 0x80) continue;
            unsigned len;
            memcpy(&len, row + 1, sizeof(unsigned));
            filesToDelete.emplace_back(row + 1 + sizeof(unsigned), len);
        }

        it.close();
        rbfm.closeFile(tFH);

        for (const auto &f : filesToDelete) rbfm.destroyFile(f);

        // in case catalog is incomplete
        rbfm.destroyFile("Tables");
        rbfm.destroyFile("Columns");
        return 0;
    }

    RC RelationManager::createTable(const std::string &tableName, const std::vector<Attribute> &attrs) {
        if (tableName == "Tables" || tableName == "Columns") return -1;

        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle tFH, cFH;

        if (rbfm.openFile("Tables", tFH) != 0) return -1;
        if (rbfm.openFile("Columns", cFH) != 0) {
            rbfm.closeFile(tFH);
            return -1;
        }

        auto tablesSchema = getTablesSchema();
        auto columnsSchema = getColumnsSchema();

        // single scan, compute next id and also check for duplicates
        RBFM_ScanIterator it;
        std::vector<std::string> proj = {"table-id", "table-name"};
        if (rbfm.scan(tFH, tablesSchema, "", NO_OP, nullptr, proj, it) != 0) {
            rbfm.closeFile(tFH);
            rbfm.closeFile(cFH);
            return -1;
        }

        int maxId = 0;
        RID rid;
        char row[PAGE_SIZE];

        while(it.getNextRecord(rid, row) != RBFM_EOF) {
            //[1 byt null bitmap][4 byte int table id][4 byte len][...chars]
            char *p = row + 1; // skip null bitmap
            int id;
            memcpy(&id, p, sizeof(int));
            p += sizeof(int);
            unsigned nameLen;
            memcpy(&nameLen, p, sizeof(unsigned));
            p+= sizeof(unsigned);
            std::string name(p, nameLen);

            if (name == tableName) {
                it.close();
                rbfm.closeFile(tFH);
                rbfm.closeFile(cFH);
                return -1;
            }

            if (id > maxId) maxId = id;
        }

        it.close();
        int nextId = maxId + 1;

        // create new table's file
        if (rbfm.createFile(tableName) != 0) {
            rbfm.closeFile(tFH);
            rbfm.closeFile(cFH);
            return -1;
        }

        // one row into Tables
        char buf[PAGE_SIZE];
        serializeTablesRow(buf, nextId, tableName, tableName);
        if (rbfm.insertRecord(tFH, tablesSchema, buf, rid) != 0) {
            rbfm.destroyFile(tableName);
            rbfm.closeFile(tFH);
            rbfm.closeFile(cFH);
            return -1;
        }

        // one row per attribute into Columns
        for (int i = 0; i < (int)attrs.size(); i++) {
            const auto &a = attrs[i];
            serializeColumnsRow(buf, nextId, a.name, a.type, a.length, i + 1);

            if (rbfm.insertRecord(cFH, columnsSchema, buf, rid) != 0) {
                rbfm.destroyFile(tableName);
                rbfm.closeFile(tFH);
                rbfm.closeFile(cFH);
                return -1;
            }
        }

        rbfm.closeFile(tFH);
        rbfm.closeFile(cFH);
        return 0;
    }

    RC RelationManager::deleteTable(const std::string &tableName) {
        if (tableName == "Tables" || tableName == "Columns") return -1;

        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle tFH, cFH;
        if (rbfm.openFile("Tables",  tFH) != 0) return -1;
        if (rbfm.openFile("Columns", cFH) != 0) { rbfm.closeFile(tFH); return -1; }

        auto tablesSchema  = getTablesSchema();
        auto columnsSchema = getColumnsSchema();

        // find Tables row
        // [4-byte length][chars]
        char nameVal[PAGE_SIZE];
        unsigned nameLen = tableName.size();
        memcpy(nameVal, &nameLen, sizeof(unsigned));
        memcpy(nameVal + sizeof(unsigned), tableName.data(), nameLen);

        RBFM_ScanIterator tIt;
        std::vector<std::string> projTables = {"table-id"};
        if (rbfm.scan(tFH, tablesSchema, "table-name", EQ_OP, nameVal, projTables, tIt) != 0) {
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }

        RID tablesRid;
        int tableId = -1;
        char row[PAGE_SIZE];
        if (tIt.getNextRecord(tablesRid, row) == RBFM_EOF) {
            tIt.close();
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }
        // [1-byte null bitmap][4-byte int]
        memcpy(&tableId, row + 1, sizeof(int));
        tIt.close();

        // collect all Columns RIDs matching this table-id
        RBFM_ScanIterator cIt;
        std::vector<std::string> projCols = {"table-id"};
        if (rbfm.scan(cFH, columnsSchema, "table-id", EQ_OP, &tableId, projCols, cIt) != 0) {
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }
        std::vector<RID> columnRids;
        RID r;
        while (cIt.getNextRecord(r, row) != RBFM_EOF) {
            columnRids.push_back(r);
        }
        cIt.close();

        rbfm.deleteRecord(tFH, tablesSchema, tablesRid);
        for (const auto &cr : columnRids) {
            rbfm.deleteRecord(cFH, columnsSchema, cr);
        }

        // destroy the user >:)
        rbfm.closeFile(tFH);
        rbfm.closeFile(cFH);
        rbfm.destroyFile(tableName);
        return 0;
    }

    RC RelationManager::getAttributes(const std::string &tableName, std::vector<Attribute> &attrs) {
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle tFH, cFH;
        if (rbfm.openFile("Tables",  tFH) != 0) return -1;
        if (rbfm.openFile("Columns", cFH) != 0) { rbfm.closeFile(tFH); return -1; }

        auto tablesSchema  = getTablesSchema();
        auto columnsSchema = getColumnsSchema();
        char nameVal[PAGE_SIZE];
        unsigned nameLen = tableName.size();
        memcpy(nameVal, &nameLen, sizeof(unsigned));
        memcpy(nameVal + sizeof(unsigned), tableName.data(), nameLen);

        // scan tables table to find the table we need the attributes for
        RBFM_ScanIterator tIt;
        std::vector<std::string> projTables = {"table-id"};
        if (rbfm.scan(tFH, tablesSchema, "table-name", EQ_OP, nameVal, projTables, tIt) != 0) {
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }

        RID tablesRid;
        int tableId = -1;
        char row[PAGE_SIZE];
        if (tIt.getNextRecord(tablesRid, row) == RBFM_EOF) {
            tIt.close();
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }
        // [1-byte null bitmap][4-byte int]
        memcpy(&tableId, row + 1, sizeof(int));
        tIt.close();

        // scan the table using table id to find the columns we need
        RBFM_ScanIterator cIt;
        std::vector<std::string> projCols = {"column-name", "column-type", "column-length", "column-position"};
        if (rbfm.scan(cFH, columnsSchema, "table-id", EQ_OP, &tableId, projCols, cIt) != 0) {
            rbfm.closeFile(tFH); rbfm.closeFile(cFH);
            return -1;
        }
        std::vector<std::pair<int, Attribute>> posColumns;
        RID r;
        while (cIt.getNextRecord(r, row) != RBFM_EOF) {
            // get the record and then first column is always 
            // skip one byte because we are projecting five fields so the bitmap is only 1 byte
            char *p = row + 1;
            unsigned charLen;
            memcpy(&charLen, p, sizeof(unsigned));
            p += sizeof(unsigned);
            std::string colName(p, charLen);
            p += charLen;
            int column_type;
            int column_length;
            int column_position;
            memcpy(&column_type, p, 4);
            p += 4;
            memcpy(&column_length, p, 4);
            p += 4;
            memcpy(&column_position, p, 4);
            p += 4;
            posColumns.push_back(std::make_pair(column_position, Attribute{colName, (AttrType)column_type, (AttrLength)column_length}));
        }
        // sort positions are not always in order
        std::sort(posColumns.begin(), posColumns.end(), [](const std::pair<int, Attribute> &a, const std::pair<int, Attribute> &b) {
            return a.first < b.first;
        });

        for (int i = 0; i < posColumns.size(); i++) {
            attrs.push_back(posColumns[i].second);
        }
        cIt.close();
        return 0;
    }

    // eveything below this is lowkey the same, make sure they dont edit the metadata tbales, get attributes and call rbfm function
    RC RelationManager::insertTuple(const std::string &tableName, const void *data, RID &rid) {
        if (tableName == "Tables" || tableName == "Columns") return -1;
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, fileHandle) != 0) return -1;
        if (rbfm.insertRecord(fileHandle, attrs, data, rid) != 0) {
            rbfm.closeFile(fileHandle);
            return -1;
        }
        rbfm.closeFile(fileHandle);
        return 0;
    }

    RC RelationManager::deleteTuple(const std::string &tableName, const RID &rid) {
        if (tableName == "Tables" || tableName == "Columns") return -1;
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, fileHandle) != 0) return -1;
        if (rbfm.deleteRecord(fileHandle, attrs, rid) != 0) {
            rbfm.closeFile(fileHandle);
            return -1;
        }
        rbfm.closeFile(fileHandle);
        return 0;
    }

    RC RelationManager::updateTuple(const std::string &tableName, const void *data, const RID &rid) {
        if (tableName == "Tables" || tableName == "Columns") return -1;
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, fileHandle) != 0) return -1;
        if (rbfm.updateRecord(fileHandle, attrs, data, rid) != 0) {
            rbfm.closeFile(fileHandle);
            return -1;
        }
        return 0;
    }

    RC RelationManager::readTuple(const std::string &tableName, const RID &rid, void *data) {
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, fileHandle) != 0) return -1;
        if (rbfm.readRecord(fileHandle, attrs, rid, data) != 0) {
            return -1;
        }
        return 0;
    }

    RC RelationManager::printTuple(const std::vector<Attribute> &attrs, const void *data, std::ostream &out) {
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        rbfm.printRecord(attrs, data, out);
        return 0;
    }

    RC RelationManager::readAttribute(const std::string &tableName, const RID &rid, const std::string &attributeName,
                                      void *data) {
        auto &rbfm = RecordBasedFileManager::instance();
        FileHandle fileHandle;
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, fileHandle) != 0) return -1;
        if (rbfm.readAttribute(fileHandle, attrs, rid, attributeName, data) != 0) {
            return -1;
        }
        rbfm.closeFile(fileHandle);
        return 0;
    }

    RC RelationManager::scan(const std::string &tableName,
                             const std::string &conditionAttribute,
                             const CompOp compOp,
                             const void *value,
                             const std::vector<std::string> &attributeNames,
                             RM_ScanIterator &rm_ScanIterator) {
        rm_ScanIterator.close();
        auto &rbfm = RecordBasedFileManager::instance();
        std::vector<Attribute> attrs;
        if (getAttributes(tableName, attrs) != 0) return -1;
        if (rbfm.openFile(tableName, rm_ScanIterator.fileHandle) != 0) return -1;
        rm_ScanIterator.isOpen = true;
        return rbfm.scan(rm_ScanIterator.fileHandle, attrs, conditionAttribute, compOp, value, attributeNames, rm_ScanIterator.rbfmScanner);
    }

    RM_ScanIterator::RM_ScanIterator() = default;

    RM_ScanIterator::~RM_ScanIterator() = default;

    RC RM_ScanIterator::getNextTuple(RID &rid, void *data) { 
        return rbfmScanner.getNextRecord(rid, data);
    }

    RC RM_ScanIterator::close() { 
        auto &rbfm = RecordBasedFileManager::instance();
        if (isOpen) {
            rbfm.closeFile(fileHandle);
            isOpen = false;
        }
        return rbfmScanner.close();

    }

    // Extra credit work
    RC RelationManager::dropAttribute(const std::string &tableName, const std::string &attributeName) {
        return -1;
    }

    // Extra credit work
    RC RelationManager::addAttribute(const std::string &tableName, const Attribute &attr) {
        return -1;
    }

    // QE IX related
    RC RelationManager::createIndex(const std::string &tableName, const std::string &attributeName){
        return -1;
    }

    RC RelationManager::destroyIndex(const std::string &tableName, const std::string &attributeName){
        return -1;
    }

    // indexScan returns an iterator to allow the caller to go through qualified entries in index
    RC RelationManager::indexScan(const std::string &tableName,
                 const std::string &attributeName,
                 const void *lowKey,
                 const void *highKey,
                 bool lowKeyInclusive,
                 bool highKeyInclusive,
                 RM_IndexScanIterator &rm_IndexScanIterator){
        return -1;
    }


    RM_IndexScanIterator::RM_IndexScanIterator() = default;

    RM_IndexScanIterator::~RM_IndexScanIterator() = default;

    RC RM_IndexScanIterator::getNextEntry(RID &rid, void *key){
        return -1;
    }

    RC RM_IndexScanIterator::close(){
        return -1;
    }

    // private helpers, return attribute lists of Tables/Columns
    std::vector<Attribute> RelationManager::getTablesSchema() {
        return {
            {"table-id", TypeInt, 4},
            {"table-name", TypeVarChar, 50},
            {"file-name", TypeVarChar, 50}
        };
    }

    std::vector<Attribute> RelationManager::getColumnsSchema() {
        return {
            {"table-id", TypeInt, 4},
            {"column-name", TypeVarChar, 50},
            {"column-type", TypeInt, 4},
            {"column-length", TypeInt, 4},
            {"column-position", TypeInt, 4}
        };
    }   

    void RelationManager::serializeTablesRow(char* buf, int tableId, const std::string &tableName,
                                const std::string &fileName) {
        char *p = buf;
        *p = 0; p += 1;

        memcpy(p, &tableId, sizeof(int));
        p += sizeof(int);

        unsigned tableLen = tableName.size();
        memcpy(p, &tableLen, sizeof(unsigned));
        p += sizeof(unsigned);
        memcpy(p, tableName.data(), tableLen);
        p += tableLen;

        unsigned fileLen = fileName.size();
        memcpy(p, &fileLen, sizeof(unsigned));
        p += sizeof(unsigned);
        memcpy(p, fileName.data(), fileLen);
    }

    void RelationManager::serializeColumnsRow(char* buf, int tableId, const std::string &colName,
                                int colType, int colLength, int colPosition) {
        char *p = buf;
        *p = 0; p += 1;

        memcpy(p, &tableId, sizeof(int));
        p += sizeof(int); // advance pointer each time

        unsigned nameLen = colName.size();
        memcpy(p, &nameLen, sizeof(unsigned));
        p += sizeof(unsigned);
        memcpy(p, colName.data(), nameLen);
        p += nameLen;

        memcpy(p, &colType, sizeof(int));
        p += sizeof(int);
        memcpy(p, &colLength, sizeof(int));
        p += sizeof(int);
        memcpy(p, &colPosition, sizeof(int));
    }


} // namespace PeterDB