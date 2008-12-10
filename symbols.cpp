/*++

	This is the part of NGdbg kernel debugger

	symbols.cpp

	This file contains routines that work with symbol tables.
	Routines can look up symbol by name or lookup symbol by address.

--*/

#include <ntifs.h>
#include "symbols.h"
#include "winnt.h"

#define NTSTRSAFE_LIB_IMPL
#include <ntstrsafe.h>

#pragma warning(disable:4995)

//
// Symbol information
//

typedef struct SYMINFO
{
	ULONG NextEntryDelta;
	ULONG SymOffset;
	char SymName[1];
} *PSYMINFO;

//
// .sym file structure
//

typedef struct LOADED_SYMBOLS
{
	ULONG TimeDateStamp;
	SYMINFO SymInfo[1];
} *PLOADED_SYMBOLS;

//
// Loaded symbols
//

typedef struct MOD_SYM
{
	LIST_ENTRY SymListEntry;
	CHAR ModName[32];
	PLOADED_SYMBOLS LoadedSymbols;
	PVOID ImageBase;
	PMDL Mdl;
	ULONG_PTR iMappedSymbols;
} *PMOD_SYM;

//
// In MP system we have to use KSPIN_LOCK
//
FAST_MUTEX SymListLock;
LIST_ENTRY SymListHead;

extern "C"
PVOID
EngMapFile (
	PWSTR,
	ULONG,
	ULONG_PTR*
	);

extern "C"
VOID
EngUnmapFile (
	ULONG_PTR
	);

HANDLE RegOpenKey (PWSTR KeyName, ACCESS_MASK DesiredAccess);
BOOLEAN RegQueryValue (HANDLE hKey, PWSTR ValueName, ULONG Type, PVOID Buffer, ULONG *Len);
PVOID FindImage (PWSTR);
PMDL LockMem (PVOID,ULONG);
VOID UnlockMem (PMDL);

VOID
SymInitialize(
	)
{
	InitializeListHead (&SymListHead);
	ExInitializeFastMutex (&SymListLock);
}

NTSTATUS
SymLoadSymbolFile(
	IN PWSTR ModuleName,
	IN PVOID ImageBase OPTIONAL
	)
{
	KdPrint(( __FUNCTION__ " loading symbols for '%S'\n", ModuleName));

	PMOD_SYM sym = (PMOD_SYM) ExAllocatePool (NonPagedPool, sizeof(MOD_SYM));
	if (!sym)
	{
		KdPrint(( __FUNCTION__ " : failed to allocate pool\n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	PWSTR dot = wcsrchr (ModuleName, L'.');
	ULONG alen = 0;
	if (dot)
	{
		alen = min (((ULONG)dot - (ULONG)ModuleName)/2, sizeof(sym->ModName)-1);
	}
	else
	{
		alen = min (wcslen(ModuleName), sizeof(sym->ModName)-1);
	}

	char ansiModuleName[128];

	ANSI_STRING AnsiString;
	UNICODE_STRING UnicodeString;

	RtlInitUnicodeString (&UnicodeString, ModuleName);
	AnsiString.Length = 0;
	AnsiString.MaximumLength = sizeof(ansiModuleName);
	AnsiString.Buffer = ansiModuleName;

	RtlUnicodeStringToAnsiString (&AnsiString, &UnicodeString, FALSE);

	KdPrint(("ansiName %s\n", ansiModuleName));

	strncpy (sym->ModName, ansiModuleName, alen);
	sym->ModName[alen] = 0;
		

	if (ImageBase)
	{
		sym->ImageBase = ImageBase;
	}
	else
	{
		sym->ImageBase = FindImage (ModuleName);
		if (!sym->ImageBase)
		{
			KdPrint(( __FUNCTION__ " : FindImage failed for %S. Is image really loaded?\n",ModuleName));
			ExFreePool (sym);
			return STATUS_NOT_FOUND;
		}
	}

	HANDLE hKey = RegOpenKey (L"\\Registry\\Machine\\Software\\NGdbg\\Symbols", KEY_QUERY_VALUE);
	if (hKey == NULL)
	{
		KdPrint(( __FUNCTION__ " : RegOpenKey failed for Symbols key\n"));
		ExFreePool (sym);
		return STATUS_UNSUCCESSFUL;
	}

	WCHAR SymFileName[512];
	ULONG len = sizeof(SymFileName)-5;
	wcscpy (SymFileName, L"\\??\\");
	
	if (!RegQueryValue (hKey, ModuleName, REG_SZ, SymFileName+4, &len))
	{
		KdPrint(( __FUNCTION__ " : RegQueryValue failed for %S\n", ModuleName));
		ZwClose (hKey);
		ExFreePool (sym);
		return STATUS_NOT_FOUND;
	}

	ZwClose (hKey);

	sym->LoadedSymbols = (PLOADED_SYMBOLS)EngMapFile (SymFileName, 0, &sym->iMappedSymbols);
	if (sym->LoadedSymbols == NULL)
	{
		ExFreePool (sym);
		KdPrint(( __FUNCTION__ " : EngMapFile failed for %S\n", SymFileName));
		return STATUS_NOT_FOUND;
	}

	// Lock symbols in memory
	ULONG Size = 4;

	PSYMINFO info = (PSYMINFO) ((PUCHAR)sym->LoadedSymbols + 4);

	while (info->NextEntryDelta)
	{
		Size += info->NextEntryDelta;
		*(ULONG*)&info += info->NextEntryDelta;
	}

	sym->Mdl = LockMem (sym->LoadedSymbols, Size);
	if (!sym->Mdl)
	{
		KdPrint(( __FUNCTION__ " : LockMem failed for %X size %X\n", sym->LoadedSymbols, Size));
		EngUnmapFile (sym->iMappedSymbols);
		ExFreePool (sym);
		return STATUS_UNSUCCESSFUL;
	}

	PIMAGE_NT_HEADERS pSymNtHeaders = (PIMAGE_NT_HEADERS)( (PUCHAR)sym->ImageBase + ((PIMAGE_DOS_HEADER)sym->ImageBase)->e_lfanew );

	if (sym->LoadedSymbols->TimeDateStamp != pSymNtHeaders->FileHeader.TimeDateStamp)
	{
		KdPrint(( __FUNCTION__ " : symbols are incorrect (sym timestamp %X mod timestamp %X)\n",
			sym->LoadedSymbols->TimeDateStamp, pSymNtHeaders->FileHeader.TimeDateStamp));
		UnlockMem (sym->Mdl);
		EngUnmapFile (sym->iMappedSymbols);
		ExFreePool (sym);
		return STATUS_UNSUCCESSFUL;
	}

	ExAcquireFastMutex (&SymListLock);
	InsertTailList (&SymListHead, &sym->SymListEntry);
	ExReleaseFastMutex (&SymListLock);

	KdPrint ((__FUNCTION__ " : symbols loaded successfully\n"));
	return STATUS_SUCCESS;
}

VOID
InternalSymUnloadSymbolTable(
	PMOD_SYM Sym
	)
{
	UnlockMem (Sym->Mdl);
	EngUnmapFile (Sym->iMappedSymbols);
	ExFreePool (Sym);
}

VOID
SymUnloadSymbolFile(
	PMOD_SYM Sym
	)
{
	KdPrint(("SymUnloadSymbolFile called for %X\n", Sym));

	ExAcquireFastMutex (&SymListLock);
	RemoveEntryList (&SymListHead);
	ExReleaseFastMutex (&SymListLock);

	InternalSymUnloadSymbolTable (Sym);
}

VOID
SymFreeSymbolTables(
	)
{
	ExAcquireFastMutex (&SymListLock);

	PMOD_SYM Sym = (PMOD_SYM) SymListHead.Flink;

	while (Sym != (PMOD_SYM) &SymListHead)
	{
		KdPrint(("Unloading symbol table %X [%s]\n", Sym, Sym->ModName));

		PMOD_SYM Flink = (PMOD_SYM) Sym->SymListEntry.Flink;

		InternalSymUnloadSymbolTable (Sym);

		Sym = Flink;
	}

	InitializeListHead (&SymListHead);

	ExReleaseFastMutex (&SymListLock);
}

NTSTATUS
SymGlobGetNearestSymbolByAddress(
	IN PVOID Address,
	OUT PCHAR Symbol,
	IN OUT ULONG *SymLen,
	OUT ULONG *Distance
	)
{
	ASSERT (KeGetCurrentIrql() >= DISPATCH_LEVEL);	// no one onws our symbol table

	NTSTATUS Status;
	ULONG MinimumDistance = 0xFFFFFFFF;
	PSYMINFO MinSymbol = NULL;
	PMOD_SYM MinModule = NULL;

	PMOD_SYM Sym = (PMOD_SYM) SymListHead.Flink;

	while (Sym != (PMOD_SYM) &SymListHead)
	{
//		KdPrint(("Searching symbol table %X [%s]\n", Sym, Sym->ModName));

		PSYMINFO Info = &Sym->LoadedSymbols->SymInfo[0];

		while (Info->NextEntryDelta)
		{
			ULONG psym = (ULONG)Sym->ImageBase + Info->SymOffset;
			ULONG dist = (ULONG)Address - (ULONG)psym;

			if (dist < MinimumDistance)
			{
				MinimumDistance = dist;
				MinSymbol = Info;
				MinModule = Sym;
			}

			*(ULONG*)&Info += Info->NextEntryDelta;
		}

		Sym = (PMOD_SYM) Sym->SymListEntry.Flink;
	}

	ULONG len = strlen(MinModule->ModName) + 1 + MinSymbol->NextEntryDelta - FIELD_OFFSET (SYMINFO,SymName);
	if (*SymLen < len)
	{
		Status = STATUS_BUFFER_OVERFLOW;
		goto exit;
	}

	strcpy (Symbol, MinModule->ModName);
	strcat (Symbol, "!");
	strcat (Symbol, MinSymbol->SymName);
	Symbol[len] = 0;

//	KdPrint(("Found sym %s\n", Symbol));

	*SymLen = len+1;
	*Distance = MinimumDistance;

exit:
	return Status;
}

NTSTATUS
SymWrGetNearestSymbolByAddress(
	IN PVOID Address,
	OUT PCHAR Symbol,
	IN OUT ULONG *SymLen
	)
{
	NTSTATUS Status;
	CHAR LocalSymbol[128];
	CHAR aDist[16];
	ULONG Dist;
	ULONG LocalLen = sizeof(LocalSymbol)-1;
	
	Status = SymGlobGetNearestSymbolByAddress (Address, LocalSymbol, &LocalLen, &Dist);
	if (NT_SUCCESS(Status))
	{
		RtlStringCchPrintfA (aDist, sizeof(aDist)-1, "+%X", Dist);

		if (*SymLen < (strlen(LocalSymbol)+strlen(aDist)+1))
			return STATUS_BUFFER_OVERFLOW;

		RtlStringCchPrintfA (Symbol, *SymLen, "%s%s", LocalSymbol, aDist);
		*SymLen = (strlen(LocalSymbol)+strlen(aDist)+1);
	}

	return Status;
}

NTSTATUS
SymGlobGetSymbolByAddress(
	IN PVOID Address,
	OUT PCHAR Symbol,
	IN OUT ULONG *SymLen
	)
{
	ASSERT (KeGetCurrentIrql() >= DISPATCH_LEVEL);	// no one onws our symbol table

	NTSTATUS Status;

	PMOD_SYM Sym = (PMOD_SYM) SymListHead.Flink;

	while (Sym != (PMOD_SYM) &SymListHead)
	{
//		KdPrint(("Searching symbol table %X [%s]\n", Sym, Sym->ModName));

		Status = SymGetSymbolByAddress (Sym, Address, Symbol, SymLen);

		if (Status == STATUS_SUCCESS)
			break;

		Sym = (PMOD_SYM) Sym->SymListEntry.Flink;
	}
	
	return Status;
}

NTSTATUS
SymGetSymbolByAddress(
	IN PMOD_SYM Sym,
	IN PVOID Address,
	OUT PCHAR Symbol,
	IN OUT ULONG *SymLen
	)

/*++

Routine Description

	Lookup symbol by address

Arguments

	LoadedSymbols

		Pointer to loaded symbol information

	ImageBase

		Base address of the image to search symbols in

	Address

		Address being looked up

	Symbol

		String receiving symbol name

	SymLen
	
		On input contains length of buffer pointed by Symbol
		On output contains number of actually written characters in Symbol

Return Value

	NTSTATUS of operation

Environment

	This function can be executed at any IRQL.
	However, symbol table should be locked in the physical memory.

--*/

{
	ULONG Offset = (ULONG)Address - (ULONG)Sym->ImageBase;
	SYMINFO* pSym = (SYMINFO*) ((PUCHAR)Sym->LoadedSymbols + 4);
	NTSTATUS Status = STATUS_NOT_FOUND;

	while (pSym->NextEntryDelta)
	{
		if (pSym->SymOffset == Offset)
		{
			//              modname            !   symbol
			ULONG len = strlen(Sym->ModName) + 1 + pSym->NextEntryDelta - FIELD_OFFSET (SYMINFO,SymName);
			if (*SymLen < len)
			{
				Status = STATUS_BUFFER_OVERFLOW;
				goto exit;
			}

			strcpy (Symbol, Sym->ModName);
			strcat (Symbol, "!");
			strcat (Symbol, pSym->SymName);
			Symbol[len] = 0;

			KdPrint(("Found sym %s\n", Symbol));

			*SymLen = len+1;

			Status = STATUS_SUCCESS;
			goto exit;
		}

		*(ULONG*)&pSym += pSym->NextEntryDelta;
	}

exit:
	return Status;
}

NTSTATUS
SymGlobGetSymbolByName(
	IN PCHAR Symbol,
	OUT ULONG *SymAddr
	)
{
	ASSERT (KeGetCurrentIrql() >= DISPATCH_LEVEL);	// no one onws our symbol table

	NTSTATUS Status;

	PMOD_SYM Sym = (PMOD_SYM) SymListHead.Flink;

	while (Sym != (PMOD_SYM) &SymListHead)
	{
//		KdPrint(("Searching symbol table %X\n", Sym));

		Status = SymGetSymbolByName (Sym, Symbol, SymAddr);

		if (Status == STATUS_SUCCESS)
			break;

		Sym = (PMOD_SYM) Sym->SymListEntry.Flink;
	}
	
	return Status;
}

NTSTATUS
SymGetSymbolByName(
	IN PMOD_SYM Sym,
	IN PCHAR Symbol,
	OUT ULONG *SymAddr
	)

/*++

Routine Description

	Lookup symbol by name

Arguments

	LoadedSymbols
	
		Pointer to loaded symbol table

	ImageBase

		Image base address

	Symbol

		Symbol name to be looked up

	SymAddr

		Receives symbol's virtual address

Return Value

	NTSTATUS of operation

Environment

	This function can be called at any IRQL
	However, symbol table should be locked in the physical memory.

--*/

{
	SYMINFO* pSym = (SYMINFO*) ((PUCHAR)Sym->LoadedSymbols + 4);
	NTSTATUS Status = STATUS_NOT_FOUND;

	while (pSym->NextEntryDelta)
	{
		if (!_strnicmp (pSym->SymName, Symbol, pSym->NextEntryDelta-FIELD_OFFSET(SYMINFO,SymName)))
		{
			*SymAddr = (ULONG)Sym->ImageBase + pSym->SymOffset;
			Status = STATUS_SUCCESS;
			goto exit;
		}

		*(ULONG*)&pSym += pSym->NextEntryDelta;
	}

exit:
	return Status;
}
