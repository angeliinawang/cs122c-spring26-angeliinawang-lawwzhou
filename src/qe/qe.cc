#include "src/include/qe.h"
#include "src/include/rbfm.h"
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace PeterDB {
    Filter::Filter(Iterator *input, const Condition &condition) {
        this->input = input;
        this->condition = condition;
        input->getAttributes(this->attrs);
    }

    Filter::~Filter() {

    }

    int getTupleSize(void *tuple, std::vector<Attribute> &attrs) {
        int fields = attrs.size();
        int nullBytes = (int)ceil(fields / 8.0);
        char *bitptr = (char *)tuple;
        char *fieldptr = (char *)tuple + nullBytes;
        for (int i = 0; i < fields; i++) {
            bool isNull = bitptr[i / 8] & (0x80 >> (i % 8));
            if (isNull) continue;
            if (attrs[i].type == TypeVarChar) {
                unsigned len;
                memcpy(&len, fieldptr, 4);
                fieldptr += 4 + len;
            } else {
                fieldptr += attrs[i].length;
            }
        }
        return (int)(fieldptr - (char *)tuple);
    }

    int findAttrIndex(std::vector<Attribute> &attrs, const std::string &fullName) {
        for (int i = 0; i < (int)attrs.size(); i++) {
            if (attrs[i].name == fullName) {
                return i;
            }
        }
        return -1;
    }

    bool compareValues(void *fieldVal, void *condVal, AttrType type, CompOp compOp) {
        // adapted from rbfm.cc
        if (type == TypeInt) {
            int a, b;
            memcpy(&a, fieldVal, 4);
            memcpy(&b, condVal, 4);
            switch (compOp) {
                case EQ_OP: return a == b;
                case GT_OP: return a > b;
                case LT_OP: return a < b;
                case GE_OP: return a >= b;
                case LE_OP: return a <= b;
                case NE_OP: return a != b;
                default: return false;
            }
        } else if (type == TypeReal) {
            float a, b;
            memcpy(&a, fieldVal, 4);
            memcpy(&b, condVal, 4);
            switch (compOp) {
                case EQ_OP: return a == b;
                case GT_OP: return a > b;
                case LT_OP: return a < b;
                case GE_OP: return a >= b;
                case LE_OP: return a <= b;
                case NE_OP: return a != b;
                default: return false;
            }
        } else {
            // TypeVarChar — both in external format: 4-byte length + chars
            unsigned int fieldLen, condLen;
            memcpy(&fieldLen, fieldVal, 4);
            memcpy(&condLen, condVal, 4);
            int cmp = strncmp((char *)fieldVal + 4, (char *)condVal + 4,
                              std::min(fieldLen, condLen));
            if (cmp == 0) cmp = (int)fieldLen - (int)condLen;
            switch (compOp) {
                case EQ_OP: return cmp == 0;
                case GT_OP: return cmp > 0;
                case LT_OP: return cmp < 0;
                case GE_OP: return cmp >= 0;
                case LE_OP: return cmp <= 0;
                case NE_OP: return cmp != 0;
                default: return false;
            }
        }
    }

    void *getFieldPtr(void *tuple, std::vector<Attribute> &attrs, int index) {
        // adpated from print record
        int fields = attrs.size();
        int nullBytes = (int)ceil(fields / 8.0);
        char *bitptr = (char *)tuple;
        char *fieldptr = (char *)tuple + nullBytes;
        bool isNull = bitptr[index / 8] & (0x80 >> (index % 8));
        if (isNull) {
            return nullptr; // field we are looking for is null so just send it back
        }
        for (int i = 0; i < index; i++) {
            if (bitptr[i / 8] & (0x80 >> (i % 8))) 
            {
                continue; // skip null fields
            }  
            if (attrs[i].type == TypeVarChar) {
            unsigned len;
            memcpy(&len, fieldptr, 4);
            fieldptr += 4 + len;
        } else {
            fieldptr += attrs[i].length;
        }
        }
        return fieldptr;
    }

    bool Filter::checkCondition(void* tuple) {
        // get left
        int lhsidx = findAttrIndex(attrs, condition.lhsAttr);
        if (lhsidx == -1) {
            return false;
        }

        void *lhsptr = getFieldPtr(tuple, attrs, lhsidx);

        if (lhsptr == nullptr) {
            return false;
        }

        if (!condition.bRhsIsAttr) {
            return compareValues(lhsptr, condition.rhsValue.data, condition.rhsValue.type,  condition.op);
        }

        // get right
        int rhsidx = findAttrIndex(attrs, condition.rhsAttr);
        if (rhsidx == -1) {
            return false;
        }

        void *rhsptr = getFieldPtr(tuple, attrs, rhsidx);
        if (rhsptr == nullptr) {
            return false;
        }

        return compareValues(lhsptr, rhsptr, attrs[lhsidx].type, condition.op);
    }

    int projectTuple(void *inputTuple, std::vector<Attribute> &inputAttrs, std::vector<std::string> &projNames, std::vector<Attribute> &outAttrs, void *outputTuple) {
        // adapted from print record, instead it just checks for the attributes we need based
        // on index
        // basically just keep track of where we are in our output as we add the desired attributes
        // then for every attribute we need, we find what index it's at, and then we check if its null,
        // and then we just grab wahtever is there using getFieldptr based on its index
        int outFields = outAttrs.size();
        int nullBytes = (int)ceil(outFields / 8.0);
        char *outBitptr = (char *)outputTuple;
        char *outFieldptr = (char *)outputTuple + nullBytes;
        memset(outBitptr, 0, nullBytes);
        for (int i = 0; i < outFields; i++) {
            int inIdx = findAttrIndex(inputAttrs, projNames[i]);
            if (inIdx == -1) {
                return -1;
            }
            char *inBitptr = (char *)inputTuple;
            bool isNull = inBitptr[inIdx / 8] & (0x80 >> (inIdx % 8));
            if (isNull) {
                outBitptr[i / 8] |= (0x80 >> (i % 8));
            } else {
                void *src = getFieldPtr(inputTuple, inputAttrs, inIdx);
                if (outAttrs[i].type == TypeVarChar) {
                    unsigned len;
                    memcpy(&len, src, 4);
                    memcpy(outFieldptr, src, 4 + len);
                    outFieldptr += 4 + len;
                } 
                else {
                    memcpy(outFieldptr, src, outAttrs[i].length);
                    outFieldptr += outAttrs[i].length;
                }
            }
        }
        return 0;
    }

    int concatTuples(void *leftTuple, void *rightTuple, std::vector<Attribute> &leftAttrs, std::vector<Attribute> &rightAttrs, void *outputTuple) {
        // asame thing as project tuple we just do it on left and right
        int outFields = leftAttrs.size() + rightAttrs.size();
        int nullBytes = (int)ceil(outFields / 8.0);
        char *outBitptr = (char *)outputTuple;
        char *outFieldptr = (char *)outputTuple + nullBytes;
        memset(outBitptr, 0, nullBytes);
        for (int i = 0; i < (int)leftAttrs.size(); i++) {
            char *inBitptr = (char *)leftTuple;
            bool isNull = inBitptr[i / 8] & (0x80 >> (i % 8));
            if (isNull) {
                outBitptr[i / 8] |= (0x80 >> (i % 8));
            } 
            else {
                void *src = getFieldPtr(leftTuple, leftAttrs, i);
                if (leftAttrs[i].type == TypeVarChar) {
                    unsigned len;
                    memcpy(&len, src, 4);
                    memcpy(outFieldptr, src, 4 + len);
                    outFieldptr += 4 + len;
                } 
                else {
                    memcpy(outFieldptr, src, leftAttrs[i].length);
                    outFieldptr += leftAttrs[i].length;
                }
            }
        }
        int rightOffset = (int)leftAttrs.size();
        for (int i = 0; i < (int)rightAttrs.size(); i++) {
            int outIdx = rightOffset + i;
            char *inBitptr = (char *)rightTuple;
            bool isNull = inBitptr[i / 8] & (0x80 >> (i % 8));
            if (isNull) {
                outBitptr[outIdx / 8] |= (0x80 >> (outIdx % 8));
            }
            else {
                void *src = getFieldPtr(rightTuple, rightAttrs, i);
                if (rightAttrs[i].type == TypeVarChar) {
                    unsigned len;
                    memcpy(&len, src, 4);
                    memcpy(outFieldptr, src, 4 + len);
                    outFieldptr += 4 + len;
                } 
                else {
                    memcpy(outFieldptr, src, rightAttrs[i].length);
                    outFieldptr += rightAttrs[i].length;
                }
            }
        }
        return 0;
    }
    

    RC Filter::getNextTuple(void *data) {
        char buffer[PAGE_SIZE];
        // get each tuple and check if it passes condition if not keep going
        while (input->getNextTuple(buffer) != QE_EOF) {
            if (checkCondition(buffer)) {
                int n = getTupleSize(buffer, attrs);
                memcpy(data, buffer, n);
                return 0;
            }
        }
        return -1;
    }

    // basically figure out the biggest tuple so we don't ever overflow our chunks
    int maxTupleSize(std::vector<Attribute> &attrs) {
        int nullBytes = (int)ceil(attrs.size() / 8.0);
        int sum = nullBytes;
        for (int i = 0; i < (int)attrs.size(); i++) {
            if (attrs[i].type == TypeVarChar) {
                sum += 4 + attrs[i].length;
            } else {
                sum += attrs[i].length;
            }
        }
        return sum;
    }

    RC Filter::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->attrs;
        return 0;
    }

    Project::Project(Iterator *input, const std::vector<std::string> &attrNames) {
        this->input = input;
        this->projAttrs = attrNames;
        input->getAttributes(this->fullAttrs);  
        // build the output attributes since it'll never change
        for (int i = 0; i < projAttrs.size(); i++) {
            int idx = findAttrIndex(fullAttrs, projAttrs[i]);
            if (idx != -1) {
                outputAttrs.push_back(fullAttrs[idx]);
            }
        }
    }

    Project::~Project() {

    }

    RC Project::getNextTuple(void *data) {
        char buffer[PAGE_SIZE];
        // get each tuple and project it
        while (input->getNextTuple(buffer) != QE_EOF) {
            int valid = projectTuple(buffer, fullAttrs, projAttrs, outputAttrs, data);
            // siutation where the projected attribute isn't in the full attributes
            if (valid == -1) {
                return -1;
            }
            return 0;
        }
        return -1;
    }

    RC Project::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->outputAttrs;
        return 0;
    }

    bool BNLJoin::loadLeftChunk() {
        int bytesUsed = 0;
        int chunkLimit = numPages * PAGE_SIZE;
        char leftTuple[PAGE_SIZE]; // stores the current tuple we got
        leftDict.clear(); // get rid of the old stuff since we are doing new chunk
        while (!leftEnd) {
            if (bytesUsed + maxSize > chunkLimit) {
                // if the next tuple is going to push us over we just stop
                break;
            }
            // get the next tuple, make sure it's not the end
            RC rc = leftIn->getNextTuple(leftTuple);
            if (rc == QE_EOF) {
                leftEnd = true;
                // means we have enough in this block
                break;
            }
            // get the size of the tuple so we know how much data to copy
            int tupleSize = getTupleSize(leftTuple, leftAttrs);
            // keyptr to point to the field we are trying to join on
            void *keyPtr = getFieldPtr(leftTuple, leftAttrs, leftKeyidx);
            std::string key;
            if (leftAttrs[leftKeyidx].type == TypeVarChar) {
                unsigned keyLen;
                memcpy(&keyLen, keyPtr, 4);
                key.assign((char *) keyPtr, 4 + keyLen);
            }
            else {
                key.assign((char *) keyPtr, leftAttrs[leftKeyidx].length);
            }

            // can't directly assign the value so make a temp and grab the left tuple stuff and then push it
            std::vector<char> temp(tupleSize);
            memcpy(temp.data(), leftTuple, tupleSize);
            leftDict[key].push_back(temp);
            bytesUsed += tupleSize;
        }
        // if its empty it means we reached thee nd
        return !leftDict.empty();
    }

    void BNLJoin::pushJoinedTuple(void *rightTuple) {
        // basically the same as how we got the left tuple, now we just look up with left
        // and then if there is a match we concat and then sned it
        void *keyPtr = getFieldPtr(rightTuple, rightAttrs, rightKeyidx);
        std::string key;
        if (rightAttrs[rightKeyidx].type == TypeVarChar) {
            unsigned keyLen;
            memcpy(&keyLen, keyPtr, 4);
            key.assign((char *) keyPtr, 4 + keyLen);
        }
        else {
            key.assign((char *) keyPtr, rightAttrs[rightKeyidx].length);
        }

        auto found = leftDict.find(key);
        if (found == leftDict.end()) {
            return;
        }

        char outputBuffer[PAGE_SIZE];
        for (std::vector<char> &leftTuple : found->second) {
            concatTuples(leftTuple.data(), rightTuple, leftAttrs, rightAttrs, outputBuffer);
            int tupleSize = getTupleSize(outputBuffer, joinedAttrs);
            std::vector<char> joinedTuples(outputBuffer, outputBuffer + tupleSize);
            output.push_back(joinedTuples);
        }
    }

    BNLJoin::BNLJoin(Iterator *leftIn, TableScan *rightIn, const Condition &condition, const unsigned int numPages) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;
        this->numPages = numPages;
        leftIn->getAttributes(leftAttrs);
        rightIn->getAttributes(rightAttrs);
        joinedAttrs = leftAttrs;
        joinedAttrs.insert(joinedAttrs.end(), rightAttrs.begin(), rightAttrs.end());
        leftKeyidx = findAttrIndex(leftAttrs, condition.lhsAttr);
        rightKeyidx = findAttrIndex(rightAttrs, condition.rhsAttr);
        maxSize = maxTupleSize(leftAttrs);
        leftEnd = false;
    }

    BNLJoin::~BNLJoin() {

    }

    RC BNLJoin::getNextTuple(void *data) {
        // idea is load left blocks into hash make a hashmap of key -> vector of left rows with that key
        // scan the right 
        // for each tuple if the key is in hash, we build the tuple of left and right
        // put it into queue to be used whenever they ask, but we should do this at the beginning in case we have leftover
        // they want one so we pop and give it to them
        // when our queue is empty we repeat the process with the next left block
        char rightTuple[PAGE_SIZE];

        // if the output is empty we load a new chunk
        while (output.empty()) {
            if (leftDict.empty()) {
                if (!loadLeftChunk()) {
                    return -1;
                }
                rightIn->setIterator();
            }
            // keep getting right tuples when we reach the end, that means we need a new left block
            RC rc = rightIn->getNextTuple(rightTuple);
            if (rc == -1) {
                leftDict.clear();
                if (leftEnd) {
                    return -1;
                }
                continue;
            }
            // this adds new output or doesn't if not then loop runs again
            pushJoinedTuple(rightTuple);
        }
        // when we have output send it one at a time
        memcpy(data, output.front().data(), output.front().size());
        output.pop_front();
        return 0;
    }

    RC BNLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->joinedAttrs;
        return 0;
    }

    INLJoin::INLJoin(Iterator *leftIn, IndexScan *rightIn, const Condition &condition) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;

        leftIn->getAttributes(leftAttrs);
        rightIn->getAttributes(rightAttrs);

        joinedAttrs = leftAttrs;
        joinedAttrs.insert(joinedAttrs.end(), rightAttrs.begin(), rightAttrs.end());

        leftKeyIdx = findAttrIndex(leftAttrs, condition.lhsAttr);
        rightKeyIdx = findAttrIndex(rightAttrs, condition.rhsAttr);

        hasCurrentLeft = false;
        leftEnd = false;
    }

    INLJoin::~INLJoin() {

    }

    RC INLJoin::getNextTuple(void *data) {
        char rightBuffer[PAGE_SIZE];
        while (true) {
            if (!hasCurrentLeft) {
                if (leftEnd) return QE_EOF;
                RC rc = leftIn->getNextTuple(currentLeft);
                if (rc == QE_EOF) { leftEnd = true; return QE_EOF; }

                // skip tuple if NULL join key
                void *leftKey = getFieldPtr(currentLeft, leftAttrs, leftKeyIdx);
                if (leftKey == nullptr) continue;

                // reset index iterator on the right side based on join operator
                switch (condition.op) {
                    case EQ_OP:
                        rightIn->setIterator(leftKey, leftKey, true, true);
                        break;
                    case LT_OP:
                        rightIn->setIterator(nullptr, leftKey, true, false);
                        break;
                    case LE_OP:
                        rightIn->setIterator(nullptr, leftKey, true, true);
                        break;
                    case GT_OP:
                        rightIn->setIterator(leftKey, nullptr, false, true);
                        break;
                    case GE_OP:
                        rightIn->setIterator(leftKey, nullptr, true, true);
                        break;
                    default:
                        rightIn->setIterator(nullptr, nullptr, true, true);
                        break;
                }
                hasCurrentLeft = true;
            }

            RC rc = rightIn->getNextTuple(rightBuffer);
            if (rc == QE_EOF) {
                hasCurrentLeft = false;
                continue;
            }

            concatTuples(currentLeft, rightBuffer, leftAttrs, rightAttrs, data);
            return 0;
        }
    }

    RC INLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->joinedAttrs;
        return 0;
    }

    std::string makeJoinKey(void *keyPtr, const Attribute &attr) {
        // making the bytes a string to use as a key
        if (attr.type == TypeVarChar) {
            unsigned len;
            memcpy(&len, keyPtr, 4);
            return std::string((char *)keyPtr, 4 + len);
        }
        return std::string((char *)keyPtr, attr.length);
    }
    int partitionIndex(const std::string &key, unsigned numPartitions) {
        // partioning the buckets, find out which one key belongs in
        std::size_t h = std::hash<std::string>{}(key);
        return (int)(h % numPartitions);
    }

    std::string GHJoin::leftPartName(int i) {
        return "left_join" + std::to_string(joinId) + "_" + std::to_string(i);
    }
    std::string GHJoin::rightPartName(int i) {
        return "right_join" + std::to_string(joinId) + "_" + std::to_string(i);
    }

    void GHJoin::partition() {
        // basically take everything and partition them into thie rbuckets
        auto &rm = RelationManager::instance();
        char buffer[PAGE_SIZE];

        // make the tmporary tables bc we are supposed to make partitions into rbfm files
        for (unsigned i = 0; i < numPartitions; i++) {
            std::string leftName = leftPartName(i);
            std::string rightName = rightPartName(i);
            rm.createTable(leftName, leftAttrs);
            rm.createTable(rightName, rightAttrs);
            partitionTables.push_back(leftName);
            partitionTables.push_back(rightName);
        }

        // partition left and then partition right into theor buckets
        while (leftIn->getNextTuple(buffer) != -1) {
            void *keyPtr = getFieldPtr(buffer, leftAttrs, leftKeyidx);
            if (keyPtr == nullptr) {
                continue; // we can't join on nulls
            }
            std::string key = makeJoinKey(keyPtr, leftAttrs[leftKeyidx]);
            int partition = partitionIndex(key, numPartitions);
            RID rid;
            std::string name = leftPartName(partition);
            rm.insertTuple(name, buffer, rid);
        }

        while (rightIn->getNextTuple(buffer) != -1) {
            void *keyPtr = getFieldPtr(buffer, rightAttrs, rightKeyidx);
            if (keyPtr == nullptr) {
                continue; // we can't join on nulls
            }
            std::string key = makeJoinKey(keyPtr, rightAttrs[rightKeyidx]);
            int partition = partitionIndex(key, numPartitions);
            RID rid;
            std::string name = rightPartName(partition);
            rm.insertTuple(name, buffer, rid);
        }

        partitioned = true;
    }

    void GHJoin::probe(int i) {
        // read all the tuples from the left in memory, then scan from right to concat when there are matches
        auto &rm = RelationManager::instance();
        std::unordered_map<std::string, std::vector<std::vector<char>>> map;

        char buffer[PAGE_SIZE];

        // scna the left tbale and make the hashmap
        std::string tableName = leftPartName(i);
        TableScan leftScan(rm, tableName);
        while (leftScan.getNextTuple(buffer) != QE_EOF) {
            void *keyPtr = getFieldPtr(buffer, leftAttrs, leftKeyidx);
            if (keyPtr == nullptr) {
                continue;
            } 
            std::string key = makeJoinKey(keyPtr, leftAttrs[leftKeyidx]);
            int tupleSize = getTupleSize(buffer, leftAttrs);
            std::vector<char> tuple(buffer, (char *)buffer + tupleSize);
            map[key].push_back(tuple);        
        }

        // use the right scan to probe the hashmap for matches

        TableScan rightScan(rm, rightPartName(i));
        char outBuffer[PAGE_SIZE];
        while (rightScan.getNextTuple(buffer) != QE_EOF) {
            void *keyPtr = getFieldPtr(buffer, rightAttrs, rightKeyidx);
            if (keyPtr == nullptr) continue;
            std::string key = makeJoinKey(keyPtr, rightAttrs[rightKeyidx]);
            auto it = map.find(key);
            if (it == map.end()) continue;
            for (std::vector<char> &leftTuple : it->second) {
                concatTuples(leftTuple.data(), buffer, leftAttrs, rightAttrs, outBuffer);
                int tupleSize = getTupleSize(outBuffer, joinedAttrs);
                std::vector<char> tuple(outBuffer, (char *)outBuffer + tupleSize);
                output.push_back(tuple);
            }
        }
    }

    GHJoin::GHJoin(Iterator *leftIn, Iterator *rightIn, const Condition &condition, const unsigned int numPartitions) {
        this->leftIn = leftIn;
        this->rightIn = rightIn;
        this->condition = condition;
        this->numPartitions = numPartitions;

        leftIn->getAttributes(leftAttrs);
        rightIn->getAttributes(rightAttrs);

        joinedAttrs = leftAttrs;
        joinedAttrs.insert(joinedAttrs.end(), rightAttrs.begin(), rightAttrs.end());

        leftKeyidx = findAttrIndex(leftAttrs, condition.lhsAttr);
        rightKeyidx = findAttrIndex(rightAttrs, condition.rhsAttr);

        // all the gh join objects need to share this so they create different files
        static int nextJoinId = 1;
        joinId = nextJoinId++;

        partitioned = false;
        currentPart = 0;

    }

    GHJoin::~GHJoin() {
        auto &rm = RelationManager::instance();
        for (const auto &name : partitionTables) {
            rm.deleteTable(name);
        }
    }

    RC GHJoin::getNextTuple(void *data) {
        // partition if it isnt
        if (!partitioned) {
            partition();
        }

        // same thing as the other join just keep giving tuples until you're out
        while (output.empty()) {
            if (currentPart >= (int)numPartitions) {
                return -1;
            }
            probe(currentPart);
            currentPart++;
        }
        // if the output still has stuff it skips and just does this regardless
        memcpy(data, output.front().data(), output.front().size());
        output.pop_front();
        return 0;

    }

    RC GHJoin::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = joinedAttrs;
        return 0;
    }

    static std::string aggregateOpName(AggregateOp op) {
        switch (op) {
            case MIN:
                return "MIN";
            case MAX:
                return "MAX";
            case COUNT:
                return "COUNT";
            case SUM:
                return "SUM";
            case AVG:
                return "AVG";
        }
        return "";
    }

    static float readNumeric(void *fieldPtr, AttrType type) {
        if (type == TypeInt) {
            int iv;
            memcpy(&iv, fieldPtr, 4);
            return (float)iv;
        }

        float fv;
        memcpy(&fv, fieldPtr, 4);
        return fv;
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, AggregateOp op) {
        this->input =input;
        this->aggAttr = aggAttr;
        this->op = op;
        this->isGroup = false;

        input->getAttributes(inputAttrs);
        aggAttrIdx = findAttrIndex(inputAttrs, aggAttr.name);
        groupAttrIdx = -1;

        Attribute out;
        out.name = aggregateOpName(op) + "(" + aggAttr.name + ")";
        out.type = TypeReal;
        out.length = 4;
        outputAttrs.push_back(out);

        computed = false;
        emitted = false;
        hasAny = false;
        minV = maxV = sumV = countV = 0;
        groupCursor = 0;
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, const Attribute &groupAttr, AggregateOp op) {
        this->input =input;
        this->aggAttr = aggAttr;
        this->groupAttr = groupAttr;
        this->op = op;
        this->isGroup = true;

        input->getAttributes(inputAttrs);
        aggAttrIdx = findAttrIndex(inputAttrs, aggAttr.name);
        groupAttrIdx = findAttrIndex(inputAttrs, groupAttr.name);

        outputAttrs.push_back(groupAttr);
        Attribute outAgg;
        outAgg.name = aggregateOpName(op) + "(" + aggAttr.name + ")";
        outAgg.type = TypeReal;
        outAgg.length = 4;
        outputAttrs.push_back(outAgg);

        computed = false;
        emitted = false;
        hasAny = false;
        minV = maxV = sumV = countV = 0;
        groupCursor = 0;
    }

    Aggregate::~Aggregate() {

    }

    RC Aggregate::getNextTuple(void *data) {
        // group by path
        if (isGroup) {
            // drain input on first call, accumulate per group
            if (!computed) {
                char buffer[PAGE_SIZE];
                while (input->getNextTuple(buffer) != QE_EOF) {
                    void *gp = getFieldPtr(buffer, inputAttrs, groupAttrIdx);
                    if (gp == nullptr) continue;

                    int gLen;
                    if (groupAttr.type == TypeVarChar) {
                        unsigned slen;
                        memcpy(&slen, gp, 4);
                        gLen = 4 + slen;
                    } else {
                        gLen = 4;
                    }

                    std::string key((char *) gp, gLen);

                    auto it = groupMap.find(key);
                    if (it == groupMap.end()) {
                        GroupAccum acc;
                        acc.keyBytes.assign((char *) gp, (char *) gp + gLen);
                        groupMap[key] = acc;
                        groupOrder.push_back(key);
                        it = groupMap.find(key);
                    }

                    GroupAccum &acc = it->second;

                    void *fp = getFieldPtr(buffer, inputAttrs, aggAttrIdx);
                    if (fp == nullptr) continue;

                    float v = readNumeric(fp, aggAttr.type);
                    if (!acc.hasAny) {
                        acc.minV = acc.maxV = v;
                        acc.hasAny = true;
                    } else {
                        if (v < acc.minV) {
                           acc.minV = v;
                        }
                        if (v > acc.maxV) {
                            acc.maxV = v;
                        }
                    }

                    acc.sumV += v;
                    acc.countV++;
                }

                computed = true;
            }

            if (groupCursor >= groupOrder.size()) {
                return QE_EOF;
            }

            GroupAccum &acc = groupMap[groupOrder[groupCursor++]];

            char *out = (char *) data;
            out[0] = 0;
            char *p = out + 1;
            memcpy(p, acc.keyBytes.data(), acc.keyBytes.size());
            p += acc.keyBytes.size();

            float result = 0;
            switch (op) {
                case MIN:
                    result = acc.minV; break;
                case MAX:
                    result = acc.maxV; break;
                case COUNT:
                    result = acc.countV; break;
                case SUM:
                    result = acc.sumV; break;
                case AVG:
                    result = acc.countV > 0 ? acc.sumV / acc.countV : 0; break;
            }

            memcpy(p, &result, 4);
            return 0;
        }

        // single result
        if (!computed) {
            char buffer[PAGE_SIZE];
            while (input->getNextTuple(buffer) != QE_EOF) {
                void *fp = getFieldPtr(buffer, inputAttrs, aggAttrIdx);
                if (fp == nullptr) continue;  // skip NULL aggregate values
                float v = readNumeric(fp, aggAttr.type);
                if (!hasAny) { minV = maxV = v; hasAny = true; }
                else {
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                }
                sumV += v;
                countV++;
            }
            computed = true;
        }

        if (emitted) return QE_EOF;
        emitted = true;

        char *out = (char *) data;
        // empty input: NULL aggregate for min/max/sum/avg, 0 for count
        if (!hasAny && op != COUNT) {
            out[0] = 0x80;
            return 0;
        }
        out[0] = 0;
        float result = 0;
        switch (op) {
            case MIN:   result = minV; break;
            case MAX:   result = maxV; break;
            case COUNT: result = countV; break;
            case SUM:   result = sumV; break;
            case AVG:   result = countV > 0 ? sumV / countV : 0; break;
        }
        memcpy(out + 1, &result, 4);
        return 0;
    }

    RC Aggregate::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->outputAttrs;
        return 0;
    }
} // namespace PeterDB
