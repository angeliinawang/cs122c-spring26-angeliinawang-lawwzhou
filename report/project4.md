## Project 4 Report


### 1. Basic information
 - Team #: 7
 - Github Repo Link: https://github.com/angeliinawang/cs122c-spring26-angeliinawang-lawwzhou
 - Student 1 UCI NetID: angeliw9
 - Student 1 Name: Angelina Wang
 - Student 2 UCI NetID: zhoulh
 - Student 2 Name: Lawrence Zhou


### 2. Catalog information about Index
- Show your catalog information about an index (tables, columns).
Our system stores the table meta data in two catalog files, Tabls and Columns. The index information is stored on an IX file on disk. When we create an index, we first check to see that the table and attribute exist. Then we create an index file. Then we scan the tuples that exist already and create the B+Tree index. 


### 3. Filter
- Describe how your filter works (especially, how you check the condition.)
Filter returns only tuples that satsify the condotion. GEt next tuple pulls from the existing output buffer and then sends it. For each tuple we make sure to check condition, to check it, we find the LHS attribute index. Then we use getfieldptr to find the field in that tuple. We compare the values between the LHS attribute and the RHS attribute which could be a constant or another attribute. We use a function called compareValues that supports int, real, and varchar, and the comparison operators we need.


### 4. Project
- Describe how your project works.
Project just only keeps the selected attributes from a tuple. We store the ones we want to projet, and then build a schema for the outputted attributes. For each tuple, we find the index of the attribute we want and then we copy it to the out. getNextTuple does the same thing as before it just keeps giving more one at a time until we are out. 


### 5. Block Nested Loop Join
- Describe how your block nested loop join works (especially, how you manage the given buffers.)
For joining, we take a left and right iterator and concatenate the schema to make a joinedAttrs. WE find the join key attribute indicies and then we compute max size to make sure we never load blocks that overflow. loadLeftChunk clears the left dict and reads in tuples one at a time until the block is full. We put each tuple into leftDict with its key being the join attribute and then the value is a vector of a vector of chars for duplicate keys.
getNextTuple uses a deque to maintain output and the while loop only runs when output is empty. When the leftDict is empty, it then loads in a new chunk and resets the right scan. When right scan is done it clears the dict to make it load the next chunk. Whenver we have a tuple match, we look it up using the key and then do pushJoinedTuple after concatting the left and right tuples (joining) together. Then we pop out one tuple at a time till it's exhusted. 


### 6. Index Nested Loop Join
- Describe how your index nested loop join works.
Constructor has botht he iterators and then joins the schemas into joinedAttrs and find the join key indicies. There is a hasCurrentLeft flag that tracks if a left tuple is loaded and if a right index scan is active. getNextTuple loops, if there is no current left tuple it grabs one and calls rightIn->setIterator. the left key is the low and high bound for EQ_Op. The other operators set the bound to null. Then right tuples are grabbed one at a time and concatenated with the current left tuple. After the right scan finishes, the loop gets the next left tuple and repeats. 



### 7. Grace Hash Join (If you have implemented this feature)
- Describe how your grace hash join works (especially, in-memory structure).
We first partition the the the tuples based on numPartitions into RM tables. Each tuple is hashed into its parititon based on the join key and modulo numPartitions. WE store all the partition table names to be deleted later on. For probing, we load all the tuples from the left current partition into a dictionary of string to vector of vector of char. We scan the right partitions one by one and for every match we call concat tuples and push it into output buffer. We probe partition by partition until the output is non empty and then we send one tuple back per call. We then delete all the tables after we are done to clean up the files. 



### 8. Aggregation
- Describe how your basic aggregation works.
For the basic aggregation we initialize with minV, maxV, sumV, countV and a computed flag. On the first getNExtTuple call, we scan the entire input once. getFieldPtr gets the attribute value and then we update the original variables based on the value. After that we compute the final results because we need to see every tuple before doing anything. We then output that final result.


- Describe how your group-based aggregation works. (If you have implemented this feature)
Group based Aggregation: It's basically the same but we have one accumulator per unqiue group key value. WE scan the entire input on the first call and then for each tuple, we extrac tthe value wiht getfieldPtr, and then we look it up in a map for the accumulators. If it doesn't exist yet then we make it. After the draining we have the groupMap with all the results. WE use groupCursor which is an index into the groupOrder, each call to getNextTuple will give you a result and move the cursor over. 


### 9. Implementation Detail
- Have you added your own module or source file (.cc or .h)?
  Clearly list the changes on files and CMakeLists.txt, if any.



- Other implementation details:



### 10. Member contribution (for team of two)
- Explain how you distribute the workload in team.
  Lawrence: Grace Hash Join, Selection, Projection, BNL join
  Angelina: INL Join, Relation Manager, Aggregation


### 11. Other (optional)
- Freely use this section to tell us about things that are related to the project 4, but not related to the other sections (optional)



- Feedback on the project to help improve the project. (optional)
