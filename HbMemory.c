#include "HbBit.h"
#include "HbFeedback.h"
#include "HbMemory.h"
#include "HbText.h"

/***************************************
 * Tag-based memory allocation tracking
 ***************************************/

static HbParallel_Mutex HbMemoryi_TagListMutex;
HbMemory_Tag * HbMemoryi_TagFirst, * HbMemoryi_TagLast;

void HbMemory_Init() {
	if (!HbParallel_Mutex_Init(&HbMemoryi_TagListMutex)) {
		HbFeedback_Crash("HbMemory_Init", "Failed to initialize the global tag list mutex.");
	}
	HbMemoryi_TagFirst = HbMemoryi_TagLast = NULL;
}

void HbMemory_Shutdown() {
	if (HbMemoryi_TagFirst != NULL) {
		HbFeedback_Crash("HbMemory_Shutdown", "Not all memory tags were destroyed.");
	}
	HbParallel_Mutex_Destroy(&HbMemoryi_TagListMutex);
}

HbMemory_Tag * HbMemory_Tag_Create(char const * name) {
	HbMemory_Tag * tag = malloc(sizeof(HbMemory_Tag));
	if (tag == NULL) {
		HbFeedback_Crash("HbMemory_Tag_Create", "Failed to allocate memory for a tag.");
	}
	if (!HbParallel_Mutex_Init(&tag->mutex)) {
		HbFeedback_Crash("HbMemory_Tag_Create", "Failed to initialize the mutex for a tag.");
	}
	if (name != NULL) {
		HbTextA_Copy(tag->name, HbArrayLength(tag->name), name);
	} else {
		tag->name[0] = '\0';
	}
	tag->allocationFirst = tag->allocationLast = NULL;
	tag->totalAllocatedSize = 0;

	HbParallel_Mutex_Lock(&HbMemoryi_TagListMutex);
	tag->globalTagPrevious = HbMemoryi_TagLast;
	tag->globalTagNext = NULL;
	if (HbMemoryi_TagLast != NULL) {
		HbMemoryi_TagLast->globalTagNext = tag;
	}
	HbMemoryi_TagLast = tag;
	if (HbMemoryi_TagFirst == NULL) {
		HbMemoryi_TagFirst = tag;
	}
	HbParallel_Mutex_Unlock(&HbMemoryi_TagListMutex);

	return tag;
}

void HbMemory_Tag_Destroy(HbMemory_Tag * tag, HbBool leaksAreErrors) {
	HbParallel_Mutex_Lock(&HbMemoryi_TagListMutex);
	if (tag->globalTagPrevious != NULL) {
		tag->globalTagPrevious->globalTagNext = tag->globalTagNext;
	} else {
		HbMemoryi_TagFirst = tag->globalTagNext;
	}
	if (tag->globalTagNext != NULL) {
		tag->globalTagNext->globalTagPrevious = tag->globalTagPrevious;
	} else {
		HbMemoryi_TagLast = tag->globalTagPrevious;
	}
	HbParallel_Mutex_Unlock(&HbMemoryi_TagListMutex);

	// Free all allocations, and, if needed, report leaks and crash.
	uint32_t leakCount = 0;
	HbParallel_Mutex_Lock(&tag->mutex);
	HbMemoryi_Allocation * allocation = tag->allocationFirst;
	while (allocation != NULL) {
		HbMemoryi_Allocation * allocationNext = allocation->tagAllocationNext;
		++leakCount;
		if (leaksAreErrors) {
			HbFeedback_DebugMessageForce("HbMemory_Tag_Destroy: leak at %s:%u (%zu bytes)",
					allocation->fileName, allocation->fileLine, allocation->size);
		}
		free(allocation);
		allocation = allocationNext;
	}
	HbParallel_Mutex_Unlock(&tag->mutex);
	if (leaksAreErrors && leakCount > 0) {
		HbFeedback_Crash("HbMemory_Tag_Destroy", "%u memory leaks in tag %s - see debug message log.",
				leakCount, tag->name != NULL ? tag->name : "(unnamed)");
	}

	HbParallel_Mutex_Destroy(&tag->mutex);
	free(tag);
}

void * HbMemory_DoAlloc(HbMemory_Tag * tag, size_t size, HbBool align16,
		char const * fileName, uint32_t fileLine, HbBool crashOnFailure) {
	size_t mallocSize = sizeof(HbMemoryi_Allocation) + HbAlignSize(size, 16); // Same as in DoRealloc.
	#if HbPlatform_CPU_32Bit
	if (align16) {
		// Alignment padding.
		mallocSize += 8;
	}
	#endif
	HbMemoryi_Allocation * allocation = malloc(mallocSize);
	if (allocation == NULL) {
		if (crashOnFailure) {
			HbFeedback_Crash("HbMemory_DoAlloc", "Failed to allocate %zu bytes in tag %s at %s:%u.",
					size, tag->name != NULL ? tag->name : "(unnamed)", fileName, fileLine);
		}
		return NULL;
	}

	void * memory = allocation + 1;
	#if HbPlatform_CPU_32Bit
	if (align16 && ((uintptr_t) memory & 8) != 0) {
		*((uint8_t * *) &memory) += 8;
		// Put the marker in the padding (so DoRealloc and Free can find the header).
		((HbMemoryi_Allocation *) memory - 1)->marker = HbMemoryi_Allocation_Marker_GoBack8;
	}
	#endif

	allocation->tag = tag;
	allocation->size = size;
	allocation->fileName = fileName;
	allocation->fileLine = (uint16_t) fileLine;
	allocation->marker = align16 ? HbMemoryi_Allocation_Marker_Aligned16 : HbMemoryi_Allocation_Marker_Aligned8;

	HbParallel_Mutex_Lock(&tag->mutex);
	allocation->tagAllocationPrevious = tag->allocationLast;
	allocation->tagAllocationNext = NULL;
	if (tag->allocationLast != NULL) {
		tag->allocationLast->tagAllocationNext = allocation;
	}
	tag->allocationLast = allocation;
	if (tag->allocationFirst == NULL) {
		tag->allocationFirst = allocation;
	}
	tag->totalAllocatedSize += size;
	HbParallel_Mutex_Unlock(&tag->mutex);

	return memory;
}

static inline HbMemoryi_Allocation * HbMemoryi_GetAllocation(void * memory) {
	if (memory == NULL) {
		return NULL;
	}
	HbMemoryi_Allocation * allocation = (HbMemoryi_Allocation *) memory - 1;
	#if HbPlatform_CPU_32Bit
	if (allocation->marker == HbMemoryi_Allocation_Marker_GoBack8) {
		*((uint8_t * *) &allocation) -= 8;
	}
	#endif
	if (allocation->marker != HbMemoryi_Allocation_Marker_Aligned8 &&
		allocation->marker != HbMemoryi_Allocation_Marker_Aligned16) {
		return NULL;
	}
	return allocation;
}

HbBool HbMemory_DoRealloc(void * * memory, size_t size, char const * fileName, uint32_t fileLine, HbBool crashOnFailure) {
	HbMemoryi_Allocation * allocation = HbMemoryi_GetAllocation(*memory);
	if (allocation == NULL) {
		HbFeedback_Crash("HbMemory_DoRealloc", "Tried to reallocate %p at %s:%u which was not allocated with HbMemory_Alloc.",
				*memory, fileName, fileLine);
	}

	// No need to lock here since only realloc can modify the size,
	// but reallocating the same memory on two threads at once is totally incorrect.
	size_t alignedSize = HbAlignSize(size, 16); // Same as in DoAlloc.
	size_t oldSize = allocation->size;
	if (alignedSize == HbAlignSize(oldSize, 16)) {
		// Same size - not reallocating.
		return HbTrue;
	}
	size_t mallocSize = sizeof(HbMemoryi_Allocation) + alignedSize;

	#if HbPlatform_CPU_32Bit
	HbBool align16 = allocation->marker == HbMemoryi_Allocation_Marker_Aligned16;
	HbBool wasPadded = ((uintptr_t) (allocation + 1) & 8) != 0;
	if (align16) {
		mallocSize += 8;
	}
	#endif

	HbMemory_Tag * tag = allocation->tag;

	HbParallel_Mutex_Lock(&tag->mutex);
	allocation = realloc(allocation, mallocSize);
	if (allocation == NULL) {
		HbParallel_Mutex_Unlock(&tag->mutex);
		if (crashOnFailure) {
			HbFeedback_Crash("HbMemory_DoRealloc", "Failed to reallocate %zu->%zu bytes in tag %s at %s:%u.",
					oldSize, size, tag->name != NULL ? tag->name : "(unnamed)", fileName, fileLine);
		}
		return HbFalse;
	}
	// The pointer to the allocation may now be different, so relink it.
	if (allocation->tagAllocationPrevious != NULL) {
		allocation->tagAllocationPrevious->tagAllocationNext = allocation;
	} else {
		tag->allocationFirst = allocation;
	}
	if (allocation->tagAllocationNext != NULL) {
		allocation->tagAllocationNext->tagAllocationPrevious = allocation;
	} else {
		tag->allocationLast = allocation;
	}
	allocation->size = size;
	allocation->fileName = fileName;
	allocation->fileLine = (uint16_t) fileLine;
	tag->totalAllocatedSize += size - oldSize;
	HbParallel_Mutex_Unlock(&tag->mutex);

	void * newMemory = allocation + 1;
	#if HbPlatform_CPU_32Bit
	if (align16) {
		// If the need for padding is changed, realign data.
		if (((uintptr_t) newMemory & 8) != 0) {
			if (!wasPadded) {
				// Need to insert the padding as the data is not 16-aligned anymore.
				memmove((uint8_t *) newMemory + 8, newMemory, HbMinI(oldSize, size));
				((HbMemoryi_Allocation *) newMemory - 1)->marker = HbMemoryi_Allocation_Marker_GoBack8;
			}
			*((uint8_t * *) &newMemory) += 8;
		} else {
			if (wasPadded) {
				// Need to remove the padding as the data is now 16-aligned.
				memmove(newMemory, (uint8_t *) newMemory + 8, HbMinI(oldSize, size));
			}
		}
	}
	#endif
	*memory = newMemory;

	return HbTrue;
}

size_t HbMemory_GetAllocationSize(void const * memory) {
	if (memory == NULL) {
		return 0;
	}
	HbMemoryi_Allocation * allocation = HbMemoryi_GetAllocation((void *) memory);
	if (allocation == NULL) {
		HbFeedback_Crash("HbMemory_GetAllocationSize", "Tried to get the size of %p which was not allocated with HbMemory_Alloc.", memory);
	}
	return allocation->size;
}

void HbMemory_Free(void * memory) {
	if (memory == NULL) {
		return; // To match C behavior, and for easier shutdown of things.
	}

	HbMemoryi_Allocation * allocation = HbMemoryi_GetAllocation(memory);
	if (allocation == NULL) {
		HbFeedback_Crash("HbMemory_Free", "Tried to free %p which was not allocated with HbMemory_Alloc.", memory);
	}

	HbMemory_Tag * tag = allocation->tag;
	HbParallel_Mutex_Lock(&tag->mutex);
	if (allocation->tagAllocationPrevious != NULL) {
		allocation->tagAllocationPrevious->tagAllocationNext = allocation->tagAllocationNext;
	} else {
		tag->allocationFirst = allocation->tagAllocationNext;
	}
	if (allocation->tagAllocationNext != NULL) {
		allocation->tagAllocationNext->tagAllocationPrevious = allocation->tagAllocationPrevious;
	} else {
		tag->allocationLast = allocation->tagAllocationPrevious;
	}
	tag->totalAllocatedSize -= allocation->size;
	HbParallel_Mutex_Unlock(&tag->mutex);

	free(allocation);
}

/**************************
 * Two-level dynamic array
 **************************/

void HbMemory_Array2L_DoInit(HbMemory_Array2L * array2L, HbMemory_Tag * tag, size_t elementSize,
		uint32_t pieceElementCountLog2, char const * fileName, uint32_t fileLine) {
	if (elementSize == 0 || pieceElementCountLog2 == 0) {
		HbFeedback_Crash("HbMemory_Array2L_DoInit", "Zero sizes specified at %s:%u.", fileName, fileLine);
	}
	if (pieceElementCountLog2 > (sizeof(size_t) * 8 - 1) || (HbMemory_Array2L_MaxLength >> pieceElementCountLog2) < elementSize) {
		// Would cause integer overflow.
		HbFeedback_Crash("HbMemory_Array2L_DoInit", "Too many elements per piece (2^%u, max 0x%zX) at %s:%u.",
				pieceElementCountLog2, HbMemory_Array2L_MaxLength / elementSize, fileName, fileLine);
	}
	// elementSize * (1 << pieceElementCountLog2) may still overflow, but that would be totally insane.
	array2L->tag = tag;
	array2L->elementSize = elementSize;
	array2L->pieceElementCountLog2 = pieceElementCountLog2;
	array2L->fileName = fileName;
	array2L->fileLine = fileLine;
	array2L->pieceElementIndexMask = ((size_t) 1 << pieceElementCountLog2) - 1;
	array2L->pieceCountLog2 = HbMemory_Array2L_PieceCountFewLog2;
	array2L->length = 0;
	memset(array2L->pieces.few, 0, sizeof(array2L->pieces.few));
}

void HbMemory_Array2L_Destroy(HbMemory_Array2L * array2L) {
	void * * pieces = HbMemory_Array2L_GetPieces(array2L);
	size_t pieceCount = (size_t) 1 << array2L->pieceCountLog2;
	for (uint64_t pieceIndex = 0; pieceIndex < pieceCount; ++pieceIndex) {
		HbMemory_Free(pieces[pieceIndex]);
	}
	if (array2L->pieceCountLog2 > HbMemory_Array2L_PieceCountFewLog2) {
		HbMemory_Free(array2L->pieces.many);
	}
}

void HbMemory_Array2L_ReservePiecePointers(HbMemory_Array2L * array2L, size_t elementCount) {
	if (elementCount > HbMemory_Array2L_MaxLength) {
		HbFeedback_Crash("HbMemory_Array2L_ReservePiecePointers",
				"Too many elements requested (0x%zX), max 0x%zX.", array2L, elementCount);
	}
	size_t pieceCount = (elementCount + array2L->pieceElementIndexMask) >> array2L->pieceElementCountLog2;
	if (pieceCount <= (size_t) 1 << HbMaxU32(array2L->pieceCountLog2, HbMemory_Array2L_PieceCountFewLog2)) {
		return;
	}
	uint32_t pieceCountLog2 = HbBit_Log2CeilSize(pieceCount);
	pieceCount = ((size_t) 1) << pieceCountLog2;
	size_t pieceCountOld = ((size_t) 1) << array2L->pieceCountLog2;
	if (array2L->pieceCountLog2 <= HbMemory_Array2L_PieceCountFewLog2) {
		// In a union with `few` - store in a local variable first.
		void * * pieces = HbMemory_DoAlloc(array2L->tag, pieceCount * sizeof(void * *), HbFalse,
				array2L->fileName, array2L->fileLine, HbTrue);
		memcpy(pieces, array2L->pieces.few, pieceCountOld * sizeof(void * *));
		array2L->pieces.many = pieces;
	} else {
		HbMemory_DoRealloc(&((void *) array2L->pieces.many), pieceCount * sizeof(void * *), array2L->fileName, array2L->fileLine, HbTrue);
	}
	memset(array2L->pieces.many + pieceCountOld, 0, (pieceCount - pieceCountOld) * sizeof(void * *));
	array2L->pieceCountLog2 = pieceCountLog2;
}

void HbMemory_Array2L_Resize(HbMemory_Array2L * array2L, size_t elementCount, HbBool onlyReserveMemory) {
	if (elementCount == 0) {
		return;
	}
	if (elementCount > HbMemory_Array2L_MaxLength) {
		HbFeedback_Crash("HbMemory_Array2L_Resize", "Too many elements requested (0x%zX), max 0x%zX.", array2L, elementCount);
	}
	HbMemory_Array2L_ReservePiecePointers(array2L, elementCount);
	if (!onlyReserveMemory) {
		array2L->length = elementCount;
	}
	void * * pieces = HbMemory_Array2L_GetPieces(array2L);
	size_t pieceCount = (elementCount + array2L->pieceElementIndexMask) >> array2L->pieceElementCountLog2;
	// Allocate pieces starting from the first unallocated one.
	size_t pieceIndex;
	for (pieceIndex = pieceCount - 1;; --pieceIndex) {
		if (pieces[pieceIndex] == NULL) {
			break;
		}
		if (pieceIndex == 0) {
			return;
		}
	}
	size_t pieceSize = array2L->elementSize << array2L->pieceElementCountLog2;
	for (; pieceIndex < pieceCount; ++pieceIndex) {
		pieces[pieceIndex] = HbMemory_DoAlloc(array2L->tag, pieceSize, HbTrue, array2L->fileName, array2L->fileLine, HbTrue);
	}
}

void const * HbMemory_Array2L_GetC(HbMemory_Array2L const * array2L, size_t offset, size_t * remainingInPiece) {
	if (offset >= array2L->length) {
		// End of iteration.
		if (remainingInPiece != NULL) {
			*remainingInPiece = 0;
		}
		return NULL;
	}
	size_t pieceElementIndex = offset & array2L->pieceElementIndexMask;
	if (remainingInPiece) {
		*remainingInPiece = HbMinSize(array2L->pieceElementIndexMask + 1 - pieceElementIndex, array2L->length - offset);
	}
	return (uint8_t const *) HbMemory_Array2L_GetPiecesC(array2L)[offset >> array2L->pieceElementCountLog2] +
			pieceElementIndex * array2L->elementSize;
}

void HbMemory_Array2L_RemoveUnsorted(HbMemory_Array2L * array2L, size_t index) {
	if (index + 1 < array2L->length) {
		memcpy(HbMemory_Array2L_Get(array2L, index, NULL), HbMemory_Array2L_Get(array2L, array2L->length - 1, NULL), array2L->elementSize);
	}
	--array2L->length;
}

void HbMemory_Array2L_FillBytes(HbMemory_Array2L * array2L, size_t offset, size_t elementCount, uint8_t value) {
	if (offset > array2L->length || array2L->length - offset < elementCount) {
		HbFeedback_Crash("HbMemory_Array2L_FillBytes", "Tried to fill %zu elements starting from %u, while the array is %zu elements long.",
				elementCount, offset, array2L->length);
	}
	while (elementCount > 0) {
		size_t memsetElementCount;
		void * elements = HbMemory_Array2L_Get(array2L, offset, &memsetElementCount);
		memsetElementCount = HbMinSize(memsetElementCount, elementCount);
		memset(elements, value, memsetElementCount * array2L->elementSize);
		elementCount -= memsetElementCount;
	}
}

/*****************************************************************************
 * Free list-based allocation with persistent indices (locators) from Array2L
 *****************************************************************************/

void HbMemory_Pool_DoInit(HbMemory_Pool * pool, HbMemory_Tag * tag, size_t elementSize,
		uint32_t array2LPieceElementCountLog2, char const * fileName, uint32_t fileLine) {
	HbMemory_Array2L_DoInit(&pool->entries, tag, sizeof(HbMemory_Pool_Entry), array2LPieceElementCountLog2, fileName, fileLine);
	HbMemory_Array2L_DoInit(&pool->elementData, tag, elementSize, array2LPieceElementCountLog2, fileName, fileLine);
	pool->firstFree = UINT32_MAX;
}

void HbMemory_Pool_Destroy(HbMemory_Pool * pool) {
	HbMemory_Array2L_Destroy(&pool->elementData);
	HbMemory_Array2L_Destroy(&pool->entries);
}

void HbMemory_Pool_Reserve(HbMemory_Pool * pool, uint32_t elementCount) {
	HbMemory_Array2L_Resize(&pool->entries, elementCount, HbTrue);
	HbMemory_Array2L_Resize(&pool->elementData, elementCount, HbTrue);
}

void const * HbMemory_Pool_GetC(HbMemory_Pool const * pool, HbMemory_Pool_Locator locator) {
	if (locator.entryIndex >= pool->entries.length) {
		return NULL;
	}
	HbMemory_Pool_Entry const * entry =
			(HbMemory_Pool_Entry const *) HbMemory_Array2L_GetC(&pool->entries, locator.entryIndex, NULL);
	if (entry->nextFree != locator.entryIndex) {
		// Free entry.
		return NULL;
	}
	if (entry->locatorRevision != locator.revision) {
		return NULL;
	}
	return HbMemory_Array2L_GetC(&pool->elementData, locator.entryIndex, NULL);
}

HbMemory_Pool_Locator HbMemory_Pool_Alloc(HbMemory_Pool * pool) {
	HbMemory_Pool_Locator locator;
	if (pool->firstFree != UINT32_MAX) {
		locator.entryIndex = pool->firstFree;
		HbMemory_Pool_Entry * entry = (HbMemory_Pool_Entry *) HbMemory_Array2L_Get(&pool->entries, locator.entryIndex, NULL);
		locator.revision = ++entry->locatorRevision;
		pool->firstFree = entry->nextFree;
		entry->nextFree = locator.entryIndex; 
	} else {
		if (pool->entries.length >= UINT32_MAX) {
			HbFeedback_Crash("HbMemory_Pool_Alloc", "Too many elements allocated, maximum 0x%X.", UINT32_MAX);
		}
		locator.entryIndex = (uint32_t) pool->entries.length;
		HbMemory_Pool_Entry * entry = (HbMemory_Pool_Entry *) HbMemory_Array2L_Append(&pool->entries);
		locator.revision = entry->locatorRevision = 0;
		entry->nextFree = locator.entryIndex;
		HbMemory_Array2L_Append(&pool->elementData);
	}
	return locator;
}

void HbMemory_Pool_Free(HbMemory_Pool * pool, HbMemory_Pool_Locator locator) {
	HbMemory_Pool_Entry * entry = NULL;
	if (locator.entryIndex < pool->entries.length) {
		entry = (HbMemory_Pool_Entry *) HbMemory_Array2L_Get(&pool->entries, locator.entryIndex, NULL);
		if (entry->nextFree != locator.entryIndex || entry->locatorRevision != locator.revision) {
			entry = NULL;
		}
	}
	if (entry == NULL) {
		HbFeedback_Crash("HbMemory_Pool_Free", "Tried to free non-existent index %u.", locator.entryIndex);
	}
	entry->nextFree = pool->firstFree;
	pool->firstFree = locator.entryIndex;
}

/*********************************
 * Power of two (buddy) allocator
 *********************************/

void HbMemory_PO2Alloc_Init(HbMemory_PO2Alloc * allocator, HbMemory_Tag * tag,
		uint32_t largestNodeSizeLog2, uint32_t smallestNodeSizeLog2) {
	if (smallestNodeSizeLog2 > largestNodeSizeLog2) {
		HbFeedback_Crash("HbMemory_PO2Alloc_Init", "Requested smallest node size (log2 %u) larger than the largest (log2 %u).",
				smallestNodeSizeLog2, largestNodeSizeLog2);
	}
	if (largestNodeSizeLog2 > 31) {
		// Too large indexes, can't return them from Alloc and take them in Free correctly
		// (the full range plus one, for zero-size allocations, is needed).
		HbFeedback_Crash("HbMemory_PO2Alloc_Init", "Requested too many bits for indexes (%u, larger than 31).", largestNodeSizeLog2);
	}
	uint32_t deepestLevel = largestNodeSizeLog2 - smallestNodeSizeLog2;
	if (deepestLevel >= HbMemoryi_PO2Alloc_MaxLevels) {
		HbFeedback_Crash("HbMemory_PO2Alloc_Init", "Too many levels of allocations requested (%u, larger than %u).",
				deepestLevel + 1, HbMemoryi_PO2Alloc_MaxLevels);
	}
	allocator->deepestLevel = deepestLevel;
	allocator->smallestNodeSizeLog2 = smallestNodeSizeLog2;

	// Initialize all nodes to zero, with the exception of the top one (for the first allocation) -
	// it must be free (since allocating requires at least one free node on the needed level or above it).
	allocator->firstFree[0] = 0;
	memset(allocator->firstFree + 1, UINT8_MAX, sizeof(allocator->firstFree) - sizeof(allocator->firstFree[0]));
	size_t nodesSize = ((1u << (allocator->deepestLevel + 1)) - 1) * sizeof(HbMemoryi_PO2Alloc_Node);
	HbMemoryi_PO2Alloc_Node * nodes = HbMemory_Alloc(tag, nodesSize, HbFalse);
	allocator->nodes = nodes;
	nodes[0].type = HbMemoryi_PO2Alloc_Node_Type_Free;
	nodes[0].previousFreeOnLevel = nodes[0].nextFreeOnLevel = -1;
	memset(nodes + 1, 0, nodesSize - sizeof(HbMemoryi_PO2Alloc_Node));
}

void HbMemory_PO2Alloc_Destroy(HbMemory_PO2Alloc * allocator) {
	HbMemory_Free(allocator->nodes);
}

HbForceInline HbMemoryi_PO2Alloc_Node * HbMemoryi_PO2Alloc_NodeOnLevel(
		HbMemory_PO2Alloc * allocator, uint32_t level, uint32_t nodeOnLevelIndex) {
	return &allocator->nodes[((1u << level) - 1) + nodeOnLevelIndex];
}

uint32_t HbMemory_PO2Alloc_TryAlloc(HbMemory_PO2Alloc * allocator, uint32_t count) {
	uint32_t maxSmallestNodes = 1u << allocator->deepestLevel;
	if (count == 0) {
		// Return a special (valid) value for zero-size allocations.
		return maxSmallestNodes << allocator->smallestNodeSizeLog2;
	}
	uint32_t countInSmallestNodes = (count + ((1u << allocator->smallestNodeSizeLog2) - 1)) >> allocator->smallestNodeSizeLog2;
	if (countInSmallestNodes > maxSmallestNodes) {
		return HbMemory_PO2Alloc_TryAllocFailed;
	}

	// Round up and get the level on which the allocation needs to take place.
	uint32_t allocationLevel = allocator->deepestLevel - HbBit_Log2CeilU32(countInSmallestNodes);

	// Find a free node on the needed level or above it (in this case, split nodes will be created).
	uint32_t freeNodeLevel = allocationLevel;
	for (;;) {
		if (allocator->firstFree[freeNodeLevel] >= 0) {
			break;
		}
		if (freeNodeLevel == 0) {
			// No free memory.
			return HbMemory_PO2Alloc_TryAllocFailed;
		}
		--freeNodeLevel;
	}

	uint32_t nodeOnLevelIndex = (uint32_t) allocator->firstFree[freeNodeLevel];
	// Not free anymore, will change to data or split.
	allocator->firstFree[freeNodeLevel] = HbMemoryi_PO2Alloc_NodeOnLevel(allocator, freeNodeLevel, nodeOnLevelIndex)->nextFreeOnLevel;
	// If no free node directly on the needed level, bridge with split nodes - walk down the left side, mark right nodes as free.
	while (freeNodeLevel < allocationLevel) {
		HbMemoryi_PO2Alloc_NodeOnLevel(allocator, freeNodeLevel, nodeOnLevelIndex)->type = HbMemoryi_PO2Alloc_Node_Type_Split;
		// Go to the next level.
		++freeNodeLevel;
		nodeOnLevelIndex <<= 1;
		// On the next level, mark the right node of the newly created split node as free, so it can be used later.
		int32_t newSecondFree = allocator->firstFree[freeNodeLevel];
		allocator->firstFree[freeNodeLevel] = nodeOnLevelIndex ^ 1;
		if (newSecondFree >= 0) {
			HbMemoryi_PO2Alloc_NodeOnLevel(allocator, freeNodeLevel, newSecondFree)->previousFreeOnLevel = nodeOnLevelIndex ^ 1;
		}
		HbMemoryi_PO2Alloc_Node * newFreeNode = HbMemoryi_PO2Alloc_NodeOnLevel(allocator, freeNodeLevel, nodeOnLevelIndex ^ 1);
		newFreeNode->type = HbMemoryi_PO2Alloc_Node_Type_Free;
		newFreeNode->previousFreeOnLevel = -1;
		newFreeNode->nextFreeOnLevel = newSecondFree;
	}
	HbMemoryi_PO2Alloc_NodeOnLevel(allocator, allocationLevel, nodeOnLevelIndex)->type = HbMemoryi_PO2Alloc_Node_Type_Data;

	return nodeOnLevelIndex << (allocator->deepestLevel - allocationLevel + allocator->smallestNodeSizeLog2);
}

uint32_t HbMemory_PO2Alloc_Alloc(HbMemory_PO2Alloc * allocator, uint32_t count) {
	uint32_t allocation = HbMemory_PO2Alloc_TryAlloc(allocator, count);
	if (allocation == HbMemory_PO2Alloc_TryAllocFailed) {
		HbFeedback_Crash("HbMemory_PO2Alloc_Alloc", "Failed to allocate %u slots.", count);
	}
	return allocation;
}

void HbMemory_PO2Alloc_Free(HbMemory_PO2Alloc * allocator, uint32_t index) {
	uint32_t indexInSmallestNodes = index >> allocator->smallestNodeSizeLog2;
	if ((indexInSmallestNodes << allocator->smallestNodeSizeLog2) != index) {
		HbFeedback_Crash("HbMemory_PO2Alloc_Free", "Unaligned index %u specified (smallest allocation has size %u).",
				index, 1u << allocator->smallestNodeSizeLog2);
	}
	uint32_t maxSmallestNodes = 1u << allocator->deepestLevel;
	if (indexInSmallestNodes >= maxSmallestNodes) {
		if (indexInSmallestNodes == maxSmallestNodes) {
			// Special value for zero-sized allocations.
			return;
		}
		HbFeedback_Crash("HbMemory_PO2Alloc_Free", "Index %u is out of bounds (%u items can be allocated).",
				index, maxSmallestNodes << allocator->smallestNodeSizeLog2);
	}

	// Get the level to start searching for the allocation from, based on alignment.
	uint32_t level = 0;
	if (indexInSmallestNodes != 0) {
		level = allocator->deepestLevel - (uint32_t) HbBit_LowestOneU32(indexInSmallestNodes);
	}
	// Find the level on which the allocation happened.
	uint32_t nodeOnLevelIndex = indexInSmallestNodes >> (allocator->deepestLevel - level);
	for (;;) {
		HbMemoryi_PO2Alloc_Node_Type nodeType = HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, nodeOnLevelIndex)->type;
		if (nodeType == HbMemoryi_PO2Alloc_Node_Type_Data) {
			// Found an allocation with such index.
			break;
		}
		// Go deeper if reached a split node, or fail if tried to free something totally invalid.
		if (nodeType != HbMemoryi_PO2Alloc_Node_Type_Split || level >= allocator->deepestLevel) {
			HbFeedback_Crash("HbMemory_PO2Alloc_Free", "Index %u not allocated with HbMemory_PO2Alloc_Alloc.", index);
		}
		++level;
		nodeOnLevelIndex <<= 1;
	}

	// Recursively go up and mark nodes as free, merging split nodes with two free children into a single free node.
	// The top level will get special treatment because it has no buddy and must have a free node if nothing is allocated.
	while (level > 0) {
		HbMemoryi_PO2Alloc_Node * node = HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, nodeOnLevelIndex);
		HbMemoryi_PO2Alloc_Node * buddy = HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, nodeOnLevelIndex ^ 1);
		if (buddy->type != HbMemoryi_PO2Alloc_Node_Type_Free) {
			// Buddy is not free, so the parent node will still be a split node, just add the current one to the free list.
			int32_t newSecondFree = allocator->firstFree[level];
			allocator->firstFree[level] = nodeOnLevelIndex;
			if (newSecondFree >= 0) {
				HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, newSecondFree)->previousFreeOnLevel = nodeOnLevelIndex;
			}
			node->type = HbMemoryi_PO2Alloc_Node_Type_Free;
			node->previousFreeOnLevel = -1;
			node->nextFreeOnLevel = newSecondFree;
			break;
		}
		// Two free nodes - destroy and join both so the range can be reused for larger allocations.
		// Destroying a free node means unlinking it from the free list.
		node->type = HbMemoryi_PO2Alloc_Node_Type_NonExistent;
		buddy->type = HbMemoryi_PO2Alloc_Node_Type_NonExistent;
		int32_t buddyPreviousFree = buddy->previousFreeOnLevel, buddyNextFree = buddy->nextFreeOnLevel;
		if (buddyPreviousFree >= 0) {
			HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, buddyPreviousFree)->nextFreeOnLevel = buddyNextFree;
		} else {
			allocator->firstFree[level] = buddyNextFree;
		}
		if (buddyNextFree >= 0) {
			HbMemoryi_PO2Alloc_NodeOnLevel(allocator, level, buddyNextFree)->previousFreeOnLevel = buddyPreviousFree;
		}
		// The split node above will be freed or destroyed at the next iteration.
		--level;
		nodeOnLevelIndex >>= 1;
	}
	if (level == 0) {
		// Destroying the last allocation - the top level must have a free node in this case.
		HbMemoryi_PO2Alloc_Node * topNode = &allocator->nodes[0];
		topNode->type = HbMemoryi_PO2Alloc_Node_Type_Free;
		topNode->previousFreeOnLevel = topNode->nextFreeOnLevel = -1;
		allocator->firstFree[0] = 0;
	}
}
