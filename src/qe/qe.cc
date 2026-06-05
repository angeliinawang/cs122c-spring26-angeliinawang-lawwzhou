#include "src/include/qe.h"
#include <cmath>

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

    int findAttrIndex(std::vector<Attribute> &attrs, std::string &fullName) {
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

    RC Filter::getAttributes(std::vector<Attribute> &attrs) const {
        attrs = this->attrs;
        return 0;
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

    BNLJoin::BNLJoin(Iterator *leftIn, TableScan *rightIn, const Condition &condition, const unsigned int numPages) {

    }

    BNLJoin::~BNLJoin() {

    }

    RC BNLJoin::getNextTuple(void *data) {
        return -1;
    }

    RC BNLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }

    INLJoin::INLJoin(Iterator *leftIn, IndexScan *rightIn, const Condition &condition) {

    }

    INLJoin::~INLJoin() {

    }

    RC INLJoin::getNextTuple(void *data) {
        return -1;
    }

    RC INLJoin::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }

    GHJoin::GHJoin(Iterator *leftIn, Iterator *rightIn, const Condition &condition, const unsigned int numPartitions) {

    }

    GHJoin::~GHJoin() {

    }

    RC GHJoin::getNextTuple(void *data) {
        return -1;
    }

    RC GHJoin::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, AggregateOp op) {

    }

    Aggregate::Aggregate(Iterator *input, const Attribute &aggAttr, const Attribute &groupAttr, AggregateOp op) {

    }

    Aggregate::~Aggregate() {

    }

    RC Aggregate::getNextTuple(void *data) {
        return -1;
    }

    RC Aggregate::getAttributes(std::vector<Attribute> &attrs) const {
        return -1;
    }
} // namespace PeterDB
