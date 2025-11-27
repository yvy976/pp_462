

----Operational Structure----
PHASE 1. Allocate all memory required for:
		MAPPER hash tables
		MAPPER string pools

PHASE 2. For EACH FILE:
		= Master thread opens the file and determines offets based on:
				- # of threads
				- word boundaries
		
		PHASE 2.1:
		= For EACH THREAD:
				- Open up a handle to the file and seek to chunk start
				
				- PARSING/READING/MAPPING LOOP:
					Parse Word
					Copy Word to THREAD OWNED String Pool
					Hash Word

				- AGGREGATE Thread Local Maps to a Local-Rank Map (IN PROGESS)
					- Attempt to store into the hash table in parallel with lock?
					- Sequentially
