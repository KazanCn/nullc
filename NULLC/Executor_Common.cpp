#include "Executor_Common.h"

#include "CodeInfo.h"
#include "StdLib.h"
#include "nullc_debug.h"
#include "Executor.h"
#include "Executor_X86.h"
#include "Executor_LLVM.h"

namespace NULLC
{
	Linker *commonLinker = NULL;
}

void CommonSetLinker(Linker* linker)
{
	NULLC::commonLinker = linker;
}

namespace GC
{
	// Range of memory that is not checked. Used to exclude pointers to stack from marking and GC
	char	*unmanageableBase = NULL;
	char	*unmanageableTop = NULL;
}

void ClosureCreate(char* paramBase, unsigned int helper, unsigned int argument, ExternFuncInfo::Upvalue* upvalue)
{
	// Function with a list of external variables to capture
	ExternFuncInfo &func = NULLC::commonLinker->exFunctions[argument];

	// Array of upvalue lists
	ExternFuncInfo::Upvalue **externalList = NULLC::commonLinker->exCloseLists.data;

	// Function external list
	ExternLocalInfo *externals = &NULLC::commonLinker->exLocals[func.offsetToFirstLocal + func.localCount];

	// For every function external
	for(unsigned int i = 0; i < func.externCount; i++)
	{
		// Calculate aligned pointer to data
		char *dataLocation = (char*)upvalue + sizeof(ExternFuncInfo::Upvalue);

		unsigned alignment = 1 << externals[i].alignmentLog2;
		dataLocation = (char*)((uintptr_t(dataLocation) + (alignment - 1)) & ~uintptr_t(alignment - 1));

		// Coroutine locals are closed immediately
		if(externals[i].target == ~0u)
		{
			upvalue->ptr = (unsigned*)dataLocation;
		}else{
			if(externals[i].closeListID & 0x80000000)	// If external variable can be found in current scope
			{
				// Take a pointer to it
				upvalue->ptr = (unsigned int*)&paramBase[externals[i].target];
			}else{	// Otherwise, we have to get pointer from functions' existing closure
				// Pointer to previous closure is the last function parameter (offset of cmd.helper from stack frame base)
				unsigned int *prevClosure = (unsigned int*)*(uintptr_t*)(&paramBase[helper]);
				// Take pointer from inside the closure (externals[i].target is in bytes, but array is of unsigned int elements)
				upvalue->ptr = *(unsigned int**)((char*)prevClosure + externals[i].target);
			}

			// Next upvalue will be current list head
			upvalue->next = externalList[externals[i].closeListID & ~0x80000000];

			// Minimum alignement is 4
			assert(externals[i].alignmentLog2 >= 2);

			unsigned alignmentLog2Bias2 = externals[i].alignmentLog2 - 2;
			assert((1 << (alignmentLog2Bias2 + 2)) == (1 << externals[i].alignmentLog2));

			// Save variable packed alignement and size
			upvalue->aligmentAndSize = (externals[i].size & 0x3fffffff) + (unsigned(alignmentLog2Bias2) << 30u);

			// Change list head to a new upvalue
			externalList[externals[i].closeListID & ~0x80000000] = upvalue;
		}

		// Preserve pointer alignment of the next upvalue
		char *nextUpvalue = dataLocation + externals[i].size;

		alignment = NULLC_PTR_SIZE;
		nextUpvalue = (char*)((uintptr_t(nextUpvalue) + (alignment - 1)) & ~uintptr_t(alignment - 1));

		// Move to the next upvalue
		upvalue = (ExternFuncInfo::Upvalue*)nextUpvalue;
	}
}

void CloseUpvalues(char* paramBase, unsigned int depth, unsigned int argument)
{
	assert(depth);
	// Array of upvalue lists
	ExternFuncInfo::Upvalue **externalList = &NULLC::commonLinker->exCloseLists[0];
	for(unsigned i = 0; i < depth; i++)
	{
		// Current upvalue
		ExternFuncInfo::Upvalue *curr = externalList[argument + i];
		// While we have an upvalue that points to an address larger than the base (so that in recursive function call only last function upvalues will be closed)
		while(curr && ((char*)curr->ptr >= paramBase || (char*)curr->ptr < GC::unmanageableBase))
		{
			// Save pointer to next upvalue
			ExternFuncInfo::Upvalue *next = curr->next;

			// And save the size of target variable
			unsigned int alignmentAndSize = curr->aligmentAndSize;
			unsigned int alignment = 1 << ((alignmentAndSize >> 30u) + 2);
			unsigned int size = alignmentAndSize & 0x3fffffff;
			
			// Delete upvalue from list (move global list head to the next element)
			externalList[argument + i] = curr->next;

			// If target value is placed on the heap, we skip copy because it won't die
			if((char*)curr->ptr < GC::unmanageableBase || (char*)curr->ptr >= GC::unmanageableTop)
			{
				curr = next;
				continue;
			}

			// Calculate aligned pointer to copy storage
			char *copyStorage = (char*)curr + sizeof(ExternFuncInfo::Upvalue);
			copyStorage = (char*)((uintptr_t(copyStorage) + (alignment - 1)) & ~uintptr_t(alignment - 1));

			// Copy target variable data into the upvalue
			memcpy(copyStorage, curr->ptr, size);
			curr->ptr = (unsigned*)copyStorage;
			curr->next = NULL;

			// Proceed to the next upvalue
			curr = next;
		}
	}
}

unsigned ConvertFromAutoRef(unsigned int target, unsigned int source)
{
	if(source == target)
		return 1;
	while(NULLC::commonLinker->exTypes[source].baseType)
	{
		source = NULLC::commonLinker->exTypes[source].baseType;
		if(source == target)
			return 1;
	}
	return 0;
}

ExternTypeInfo*	GetTypeList()
{
	return NULLC::commonLinker->exTypes.data;
}

unsigned int PrintStackFrame(int address, char* current, unsigned int bufSize)
{
	const char *start = current;

	FastVector<ExternFuncInfo> &exFunctions = NULLC::commonLinker->exFunctions;
	FastVector<char> &exSymbols = NULLC::commonLinker->exSymbols;
	FastVector<ExternModuleInfo> &exModules = NULLC::commonLinker->exModules;

	struct SourceInfo
	{
		unsigned int byteCodePos;
		unsigned int sourceOffset;
	};

	SourceInfo *exInfo = (SourceInfo*)&NULLC::commonLinker->exCodeInfo[0];
	const char *source = &NULLC::commonLinker->exSource[0];
	unsigned int infoSize = NULLC::commonLinker->exCodeInfo.size() / 2;

	int funcID = -1;
	for(unsigned int i = 0; i < exFunctions.size(); i++)
		if(address >= exFunctions[i].address && address <= (exFunctions[i].address + exFunctions[i].codeSize))
			funcID = i;
	if(funcID != -1)
		current += SafeSprintf(current, bufSize - int(current - start), "%s", &exSymbols[exFunctions[funcID].offsetToName]);
	else
		current += SafeSprintf(current, bufSize - int(current - start), "%s", address == -1 ? "external" : "global scope");
	if(address != -1)
	{
		unsigned int infoID = 0;
		unsigned int i = address - 1;
		while((infoID < infoSize - 1) && (i >= exInfo[infoID + 1].byteCodePos))
			infoID++;
		const char *codeStart = source + exInfo[infoID].sourceOffset;
		// Find beginning of the line
		while(codeStart != source && *(codeStart-1) != '\n')
			codeStart--;
		// Skip whitespace
		while(*codeStart == ' ' || *codeStart == '\t')
			codeStart++;
		const char *codeEnd = codeStart;
		// Find corresponding module
		unsigned moduleID = ~0u;
		const char *prevEnd = NULL;
		for(unsigned l = 0; l < exModules.size(); l++)
		{
			// special check for main module
			if(source + exModules[l].sourceOffset > prevEnd && codeStart >= prevEnd && codeStart < source + exModules[l].sourceOffset)
				break;
			if(codeStart >= source + exModules[l].sourceOffset && codeStart < source + exModules[l].sourceOffset + exModules[l].sourceSize)
				moduleID = l;
			prevEnd = source + exModules[l].sourceOffset + exModules[l].sourceSize;
		}
		const char *moduleStart = NULL;
		if(moduleID != ~0u)
			moduleStart = source + exModules[moduleID].sourceOffset;
		else
			moduleStart = prevEnd;
		// Find line number
		unsigned line = 0;
		while(moduleStart < codeStart)
		{
			if(*moduleStart++ == '\n')
				line++;
		}
		// Find ending of the line
		while(*codeEnd != '\0' && *codeEnd != '\r' && *codeEnd != '\n')
			codeEnd++;
		int codeLength = (int)(codeEnd - codeStart);
		current += SafeSprintf(current, bufSize - int(current - start), " (line %d: at %.*s)\r\n", line + 1, codeLength, codeStart);
	}
#ifdef NULLC_STACK_TRACE_WITH_LOCALS
	FastVector<ExternLocalInfo> &exLocals = NULLC::commonLinker->exLocals;
	FastVector<ExternTypeInfo> &exTypes = NULLC::commonLinker->exTypes;
	if(funcID != -1)
	{
		for(unsigned int i = 0; i < exFunctions[funcID].localCount + exFunctions[funcID].externCount; i++)
		{
			ExternLocalInfo &lInfo = exLocals[exFunctions[funcID].offsetToFirstLocal + i];
			const char *typeName = &exSymbols[exTypes[lInfo.type].offsetToName];
			const char *localName = &exSymbols[lInfo.offsetToName];
			const char *localType = lInfo.paramType == ExternLocalInfo::PARAMETER ? "param" : (lInfo.paramType == ExternLocalInfo::EXTERNAL ? "extern" : "local");
			const char *offsetType = (lInfo.paramType == ExternLocalInfo::PARAMETER || lInfo.paramType == ExternLocalInfo::LOCAL) ? "base" :
				(lInfo.closeListID & 0x80000000 ? "local" : "closure");
			current += SafeSprintf(current, bufSize - int(current - start), " %s %d: %s %s (at %s+%d size %d)\r\n",
				localType, i, typeName, localName, offsetType, lInfo.offset, exTypes[lInfo.type].size);
		}
	}
#endif
	return (unsigned int)(current - start);
}

#define GC_DEBUG_PRINT(...)
//#define GC_DEBUG_PRINT printf

namespace GC
{
	unsigned int	objectName = GetStringHash("auto ref");
	unsigned int	autoArrayName = GetStringHash("auto[]");

	void CheckArray(char* ptr, const ExternTypeInfo& type);
	void CheckClass(char* ptr, const ExternTypeInfo& type);
	void CheckFunction(char* ptr);
	void CheckVariable(char* ptr, const ExternTypeInfo& type);

	struct RootInfo
	{
		RootInfo(){}
		RootInfo(char* ptr, const ExternTypeInfo* type): ptr(ptr), type(type){}

		char *ptr;
		const ExternTypeInfo* type;
	};
	FastVector<RootInfo> rootsA, rootsB;
	FastVector<RootInfo> *curr = NULL, *next = NULL;

	HashMap<int> functionIDs;

	// Function that marks memory blocks belonging to GC
	void MarkPointer(char* ptr, const ExternTypeInfo& type, bool takeSubtype)
	{
		// We have pointer to stack that has a pointer inside, so 'ptr' is really a pointer to pointer
		char **rPtr = (char**)ptr;
		// Check for unmanageable ranges. Range of 0x00000000-0x00010000 is unmanageable by default due to upvalues with offsets inside closures.
		if(*rPtr > (char*)0x00010000 && (*rPtr < unmanageableBase || *rPtr > unmanageableTop))
		{
			// Get type that pointer points to
			GC_DEBUG_PRINT("\tGlobal pointer %s %p (at %p)\r\n", NULLC::commonLinker->exSymbols.data + type.offsetToName, *rPtr, ptr);

			// Get pointer to the start of memory block. Some pointers may point to the middle of memory blocks
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(*rPtr);
			// If there is no base, this pointer points to memory that is not GCs memory
			if(!basePtr)
				return;
			GC_DEBUG_PRINT("\tPointer base is %p\r\n", basePtr);

			// Marker is before the block
			markerType	*marker = (markerType*)((char*)basePtr - sizeof(markerType));
			GC_DEBUG_PRINT("\tMarker is %d (type %d, flag %d)\r\n", *marker, *marker >> 8, *marker & 0xff);

			// If block is unmarked
			if(!(*marker & 1))
			{
				// Mark block as used
				*marker |= 1;
				// And if type is not simple, check memory to which pointer points to
				if(type.subCat != ExternTypeInfo::CAT_NONE)
					next->push_back(RootInfo(*rPtr, takeSubtype ? &NULLC::commonLinker->exTypes[type.subType] : &type));
			}
		}
	}

	// Function that checks arrays for pointers
	void CheckArray(char* ptr, const ExternTypeInfo& type)
	{
		// Get array element type
		ExternTypeInfo *subType = type.nameHash == autoArrayName ? NULL : &NULLC::commonLinker->exTypes[type.subType];
		// Real array size (changed for unsized arrays)
		unsigned int size = type.arrSize;
		// If array type is an unsized array, check pointer that points to actual array contents
		if(type.arrSize == TypeInfo::UNSIZED_ARRAY)
		{
			// Get real array size
			size = *(int*)(ptr + NULLC_PTR_SIZE);
			// Switch pointer to array data
			char **rPtr = (char**)ptr;
			ptr = *rPtr;
			// If uninitialized or points to stack memory, return
			if(!ptr || ptr <= (char*)0x00010000 || (ptr >= unmanageableBase && ptr <= unmanageableTop))
				return;
			GC_DEBUG_PRINT("\tGlobal pointer %p\r\n", ptr);
			// Get base pointer
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			markerType	*marker = (markerType*)((char*)basePtr - sizeof(markerType));
			// If there is no base pointer or memory already marked, exit
			if(!basePtr || (*marker & 1))
				return;
			// Mark memory as used
			*marker |= 1;
		}else if(type.nameHash == autoArrayName){
			NULLCAutoArray *data = (NULLCAutoArray*)ptr;
			// Get real variable type
			subType = &NULLC::commonLinker->exTypes[data->typeID];
			// skip uninitialized array
			if(!data->ptr)
				return;
			// Mark target data
			MarkPointer((char*)&data->ptr, *subType, false);
			// Switch pointer to target
			ptr = data->ptr;
			// Get array size
			size = data->len;
		}
		if(!subType->pointerCount)
			return;
		// Otherwise, check every array element is it's either array, pointer of class
		switch(subType->subCat)
		{
		case ExternTypeInfo::CAT_ARRAY:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckArray(ptr, *subType);
			break;
		case ExternTypeInfo::CAT_POINTER:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				MarkPointer(ptr, *subType, true);
			break;
		case ExternTypeInfo::CAT_CLASS:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckClass(ptr, *subType);
			break;
		case ExternTypeInfo::CAT_FUNCTION:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckFunction(ptr);
			break;
		}
	}

	// Function that checks classes for pointers
	void CheckClass(char* ptr, const ExternTypeInfo& type)
	{
		const ExternTypeInfo *realType = &type;
		if(type.nameHash == objectName)
		{
			// Get real variable type
			realType = &NULLC::commonLinker->exTypes[*(int*)ptr];
			// Switch pointer to target
			char **rPtr = (char**)(ptr + 4);
			ptr = *rPtr;
			// If uninitialized or points to stack memory, return
			if(!ptr || ptr <= (char*)0x00010000 || (ptr >= unmanageableBase && ptr <= unmanageableTop))
				return;
			// Get base pointer
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			markerType	*marker = (markerType*)((char*)basePtr - sizeof(markerType));
			// If there is no base pointer or memory already marked, exit
			if(!basePtr || (*marker & 1))
				return;
			// Mark memory as used
			*marker |= 1;
			// Fixup target
			CheckVariable(*rPtr, *realType);
			// Exit
			return;
		}else if(type.nameHash == autoArrayName){
			CheckArray(ptr, type);
			// Exit
			return;
		}
		// Get class member type list
		ExternMemberInfo *memberList = realType->pointerCount ? &NULLC::commonLinker->exTypeExtra[realType->memberOffset + realType->memberCount] : NULL;
		// Check pointer members
		for(unsigned int n = 0; n < realType->pointerCount; n++)
		{
			// Get member type
			ExternTypeInfo &subType = NULLC::commonLinker->exTypes[memberList[n].type];
			unsigned int pos = memberList[n].offset;
			// Check member
			CheckVariable(ptr + pos, subType);
		}
	}

	// Function that checks function context for pointers
	void CheckFunction(char* ptr)
	{
		NULLCFuncPtr *fPtr = (NULLCFuncPtr*)ptr;
		// If there's no context, there's nothing to check
		if(!fPtr->context)
			return;
		const ExternFuncInfo &func = NULLC::commonLinker->exFunctions[fPtr->id];
		// External functions shouldn't be checked
		if(func.address == -1)
			return;
		// If context is "this" pointer
		if(func.contextType != ~0u)
		{
			const ExternTypeInfo &classType = NULLC::commonLinker->exTypes[func.contextType];
			MarkPointer((char*)&fPtr->context, classType, true);
		}
	}

	// Function that decides, how variable of type 'type' should be checked for pointers
	void CheckVariable(char* ptr, const ExternTypeInfo& type)
	{
		const ExternTypeInfo *realType = &type;
		if(type.typeFlags & ExternTypeInfo::TYPE_IS_EXTENDABLE)
			realType = &NULLC::commonLinker->exTypes[*(int*)ptr];
		if(!realType->pointerCount)
			return;
		switch(type.subCat)
		{
		case ExternTypeInfo::CAT_ARRAY:
			CheckArray(ptr, type);
			break;
		case ExternTypeInfo::CAT_POINTER:
			MarkPointer(ptr, type, true);
			break;
		case ExternTypeInfo::CAT_CLASS:
			CheckClass(ptr, *realType);
			break;
		case ExternTypeInfo::CAT_FUNCTION:
			CheckFunction(ptr);
			break;
		}
	}
}

// Set range of memory that is not checked. Used to exclude pointers to stack from marking and GC
void SetUnmanagableRange(char* base, unsigned int size)
{
	GC::unmanageableBase = base;
	GC::unmanageableTop = base + size;
}
int IsPointerUnmanaged(NULLCRef ptr)
{
	return ptr.ptr >= GC::unmanageableBase && ptr.ptr <= GC::unmanageableTop;
}

// Main function for marking all pointers in a program
void MarkUsedBlocks()
{
	GC_DEBUG_PRINT("Unmanageable range: %p-%p\r\n", GC::unmanageableBase, GC::unmanageableTop);

	// Get information about programs' functions, variables, types and symbols (for debug output)
	ExternFuncInfo	*functions = NULLC::commonLinker->exFunctions.data;
	ExternVarInfo	*vars = NULLC::commonLinker->exVariables.data;
	ExternTypeInfo	*types = NULLC::commonLinker->exTypes.data;
	char			*symbols = NULLC::commonLinker->exSymbols.data;
	(void)symbols;

	GC::functionIDs.init();
	GC::functionIDs.clear();

	GC::curr = &GC::rootsA;
	GC::next = &GC::rootsB;
	GC::curr->clear();
	GC::next->clear();

	// To check every stack frame, we have to get it first. But we have two different executors, so flow alternates depending on which executor we are running
	void *unknownExec = NULL;
	unsigned int execID = nullcGetCurrentExecutor(&unknownExec);

	if(execID != NULLC_LLVM)
	{
		// Mark global variables
		for(unsigned int i = 0; i < NULLC::commonLinker->exVariables.size(); i++)
		{
			GC_DEBUG_PRINT("Global %s %s (with offset of %d)\r\n", symbols + types[vars[i].type].offsetToName, symbols + vars[i].offsetToName, vars[i].offset);
			GC::CheckVariable(GC::unmanageableBase + vars[i].offset, types[vars[i].type]);
		}
	}else{
#ifdef NULLC_LLVM_SUPPORT
		ExecutorLLVM *exec = (ExecutorLLVM*)unknownExec;

		unsigned count = 0;
		char *data = exec->GetVariableData(&count);

		for(unsigned int i = 0; i < NULLC::commonLinker->exVariables.size(); i++)
		{
			GC_DEBUG_PRINT("Global %s %s (with offset of %d)\r\n", symbols + types[vars[i].type].offsetToName, symbols + vars[i].offsetToName, vars[i].offset);
			GC::CheckVariable(data + vars[i].offset, types[vars[i].type]);
		}
#endif
	}

	// Starting stack offset is equal to global variable size
	int offset = NULLC::commonLinker->globalVarSize;
	
	// Init stack trace
	if(execID == NULLC_VM)
	{
		Executor *exec = (Executor*)unknownExec;
		exec->BeginCallStack();
	}
	
#ifdef NULLC_BUILD_X86_JIT
	if(execID == NULLC_X86)
	{
		ExecutorX86 *exec = (ExecutorX86*)unknownExec;
		exec->BeginCallStack();
	}
#endif

#ifdef NULLC_LLVM_SUPPORT
	if(execID == NULLC_LLVM)
	{
		ExecutorLLVM *exec = (ExecutorLLVM*)unknownExec;
		exec->BeginCallStack();
	}
#endif

	// Mark local variables
	while(true)
	{
		int address = 0;
		// Get next address from call stack
		if(execID == NULLC_VM)
		{
			Executor *exec = (Executor*)unknownExec;
			address = exec->GetNextAddress();
		}

#ifdef NULLC_BUILD_X86_JIT
		if(execID == NULLC_X86)
		{
			ExecutorX86 *exec = (ExecutorX86*)unknownExec;
			address = exec->GetNextAddress();
		}
#endif

#ifdef NULLC_LLVM_SUPPORT
		if(execID == NULLC_LLVM)
		{
			ExecutorLLVM *exec = (ExecutorLLVM*)unknownExec;
			address = exec->GetNextAddress();
		}
#endif
		// If failed, exit
		if(address == 0)
			break;

		// Find corresponding function
		int *cachedFuncID = GC::functionIDs.find(address);

		int funcID = -1;
		if(cachedFuncID)
		{
			funcID = *cachedFuncID;
		}else{
			for(unsigned int i = 0; i < NULLC::commonLinker->exFunctions.size(); i++)
			{
				if(address >= functions[i].address && address < (functions[i].address + functions[i].codeSize))
				{
					funcID = i;
				}
			}

			GC::functionIDs.insert(address, funcID);
		}

		// If we are not in global scope
		if(funcID != -1)
		{
			// Align offset to the first variable (by 16 byte boundary)
			int alignOffset = (offset % 16 != 0) ? (16 - (offset % 16)) : 0;
			offset += alignOffset;
			GC_DEBUG_PRINT("In function %s (with offset of %d)\r\n", symbols + functions[funcID].offsetToName, alignOffset);

			unsigned int offsetToNextFrame = functions[funcID].bytesToPop;
			// Check every function local
			for(unsigned int i = 0; i < functions[funcID].localCount; i++)
			{
				// Get information about local
				ExternLocalInfo &lInfo = NULLC::commonLinker->exLocals[functions[funcID].offsetToFirstLocal + i];
				if(functions[funcID].funcCat == ExternFuncInfo::COROUTINE && lInfo.offset >= functions[funcID].bytesToPop)
					break;
				GC_DEBUG_PRINT("Local %s %s (with offset of %d)\r\n", symbols + types[lInfo.type].offsetToName, symbols + lInfo.offsetToName, offset + lInfo.offset);
				// Check it
				GC::CheckVariable(GC::unmanageableBase + offset + lInfo.offset, types[lInfo.type]);
				if(lInfo.offset + lInfo.size > offsetToNextFrame)
					offsetToNextFrame = lInfo.offset + lInfo.size;
			}
			if(functions[funcID].contextType != ~0u)
			{
				GC_DEBUG_PRINT("Local %s $context (with offset of %d+%d)\r\n", symbols + types[functions[funcID].contextType].offsetToName, offset, functions[funcID].bytesToPop - NULLC_PTR_SIZE);
				char *ptr = GC::unmanageableBase + offset + functions[funcID].bytesToPop - NULLC_PTR_SIZE;
				GC::MarkPointer(ptr, types[functions[funcID].contextType], false);
			}
			offset += offsetToNextFrame;
			GC_DEBUG_PRINT("Moving offset to next frame by %d bytes\r\n", offsetToNextFrame);
		}
	}

	// Check pointers inside all unclosed upvalue lists
	for(unsigned int i = 0; i < NULLC::commonLinker->exCloseLists.size() && execID != NULLC_LLVM; i++)
	{
		// List head
		ExternFuncInfo::Upvalue *curr = NULLC::commonLinker->exCloseLists[i];
		// Move list head while it points to unused upvalue
		while(curr)
		{
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(curr);
			markerType *marker = (markerType*)((char*)basePtr - sizeof(markerType));
			if(basePtr && !(*marker & 1))
				curr = curr->next;
			else
				break;
		}
		// Change list head in global data
		NULLC::commonLinker->exCloseLists[i] = curr;
		// Delete remaining unused upvalues from list
		while(curr && curr->next)
		{
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(curr->next);
			markerType *marker = (markerType*)((char*)basePtr - sizeof(markerType));
			if(basePtr && (*marker & 1))
				curr = curr->next;
			else
				curr->next = curr->next->next;
		}
	}

	// Check for pointers in stack
	char *tempStackBase = NULL, *tempStackTop = NULL;
	if(execID == NULLC_VM)
	{
		Executor *exec = (Executor*)unknownExec;
		tempStackBase = (char*)exec->GetStackStart();
		tempStackTop = (char*)exec->GetStackEnd();
	}

#ifdef NULLC_BUILD_X86_JIT
	if(execID == NULLC_X86)
	{
		ExecutorX86 *exec = (ExecutorX86*)unknownExec;
		tempStackBase = (char*)exec->GetStackStart();
		tempStackTop = (char*)exec->GetStackEnd();
	}
#endif

#ifdef NULLC_LLVM_SUPPORT
	if(execID == NULLC_LLVM)
	{
		ExecutorLLVM *exec = (ExecutorLLVM*)unknownExec;
		tempStackBase = (char*)exec->GetStackStart();
		tempStackTop = (char*)exec->GetStackEnd();
	}
#endif

	GC_DEBUG_PRINT("Check stack from %p to %p\r\n", tempStackBase, tempStackTop);

	// Check that temporary stack range is correct
	assert(tempStackTop >= tempStackBase);
	// Check temporary stack for pointers
	while(tempStackBase < tempStackTop)
	{
		char *ptr = *(char**)(tempStackBase);
		// Check for unmanageable ranges. Range of 0x00000000-0x00010000 is unmanageable by default due to upvalues with offsets inside closures.
		if(ptr > (char*)0x00010000 && (ptr < GC::unmanageableBase || ptr > GC::unmanageableTop))
		{
			// Get pointer base
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			// If there is no base, this pointer points to memory that is not GCs memory
			if(basePtr)
			{
				markerType *marker = (markerType*)((char*)basePtr - sizeof(markerType));

				// If block is unmarked, mark it as used
				if(!(*marker & 1))
				{
					unsigned typeID = unsigned(*marker >> 8);
					ExternTypeInfo &type = types[typeID];

					*marker |= 1;

					// And if type is not simple, check memory to which pointer points to
					if(type.subCat != ExternTypeInfo::CAT_NONE)
						GC::CheckVariable((char*)basePtr, type);
				}
			}
		}
		tempStackBase += 4;
	}

	GC_DEBUG_PRINT("Checking new roots\r\n");

	while(GC::next->size())
	{
		FastVector<GC::RootInfo>	*tmp = GC::curr;
		GC::curr = GC::next;
		GC::next = tmp;

		for(GC::RootInfo *c = GC::curr->data, *e = GC::curr->data + GC::curr->size(); c != e; c++)
			GC::CheckVariable(c->ptr, *c->type);

		GC::curr->clear();
	}
}

void ResetGC()
{
	GC::rootsA.reset();
	GC::rootsB.reset();

	GC::functionIDs.reset();
}
