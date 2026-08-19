/*
Ultimap implements the recording of all the branches in the target process
Requires dbvm for process selection
*/

#pragma warning( disable: 4100 4101 4213)

#include <ntifs.h>
#include "ultimap.h"
#include "vmxhelper.h"
#include "DBKFunc.h"
#include <windef.h>

#include "ultimap2\apic.h"


JUMPBACK perfmonJumpBackLocation;


/*
#ifdef AMD64
volatile PAPIC APIC_BASE=0; //(PAPIC)0xfffffffffffe0000;
#else
volatile PAPIC APIC_BASE=0; //(PAPIC)0xfffe0000;
#endif
*/

BOOL SaveToFile; //If set it will save the results to a file instead of sending a message to the usermode app that is watching the data
HANDLE FileHandle;

int MaxDataBlocks=1;

KSEMAPHORE DataBlockSemaphore; //governs how many events can be active at a time
FAST_MUTEX DataBlockMutex; //when a thread passes the semaphore this is used to pick a DataBlock


typedef struct
{
	BOOL Available;
	PBTS Data;
	int DataSize;
	int CpuID;
	KEVENT DataReady;    
} _DataBlock;

_DataBlock *DataBlock;
PVOID *DataReadyPointerList;
volatile LONG UltimapWaiterCount;
KEVENT UltimapWaitersDrained;
BOOL UltimapStopping = TRUE;
BOOLEAN UltimapHookRegistered = FALSE;

int perfmon_interrupt_centry(void);

/* use apic.* now
#define MSR_IA32_APICBASE               0x0000001b

void setup_APIC_BASE(void)
{
	PHYSICAL_ADDRESS Physical_APIC_BASE;
	DbgPrint("Fetching the APIC base\n");

	Physical_APIC_BASE.QuadPart=readMSR(MSR_IA32_APICBASE) & 0xFFFFFFFFFFFFF000ULL;
	

	DbgPrint("Physical_APIC_BASE=%p\n", Physical_APIC_BASE.QuadPart);

	APIC_BASE = (PAPIC)MmMapIoSpace(Physical_APIC_BASE, sizeof(APIC), MmNonCached);


    DbgPrint("APIC_BASE at %p\n", APIC_BASE);

}

void clean_APIC_BASE(void)
{
	if (APIC_BASE)
		MmUnmapIoSpace((PVOID)APIC_BASE, sizeof(APIC));
}*/

void ultimap_flushBuffers_all(UINT_PTR param)
{
	DbgPrint("Calling perfmon_interrupt_centry() manually\n");
	if (DS_AREA[cpunr()]) //don't call if ultimap has been disabled
	{
		perfmon_interrupt_centry();
		enableInterrupts(); //the handler disables it on exit so re-enable it
	}
}

void ultimap_flushBuffers(void)
{
	//call this when the buffer of the current cpu needs to be flushed and handled
	int i;
	int count;

	DbgPrint("ultimap_flushBuffers\n");

	//what it does:
	//for each cpu emulate a "buffer filled" event.
	//the handler then copies all the current data to a temporary buffer and signals the worker thread to deal with it. If there is no available worker thread it waits
	forEachCpuPassive(ultimap_flushBuffers_all,0);

	DbgPrint("ultimap_flushBuffers_all has returned\n");
	//it returned and all worker thread are currently working on this data (it only returns when it has send a worker to work)


	//now wait for all workers to finish
	//do this by aquiring all semaphore slots and waiting for them to return again
	//forEachCpuPassive(ultimap_flushBuffers_all,0);
	//DbgPrint("ultimap_flushBuffers_all has returned a second time\n"); //this means that the previous blocks have been dealt with


	//actually... no, this is no guarantee. Now that the buffers are empty handling is so fast that while block 2,3,4,5 and 6 are still being handled block 1 can become available multiple times
	
}


NTSTATUS ultimap_continue(PULTIMAPDATAEVENT data)
/*
Called from usermode to signal that the data has been handled
*/
{
	DbgPrint("ultimap_continue\n");
	MmUnmapLockedPages((PVOID)(UINT_PTR)data->Address, (PMDL)(UINT_PTR)data->Mdl);
	IoFreeMdl((PMDL)(UINT_PTR)data->Mdl);

	ExFreePool((PVOID)(UINT_PTR)data->KernelAddress); //this memory is not needed anymore


	if (DataBlock)
		DataBlock[data->Block].Available=TRUE;


	KeReleaseSemaphore(&DataBlockSemaphore, 1, 1, FALSE); //Let the next block go through
	DbgPrint("Released semaphore\n");
	return STATUS_SUCCESS;	
}

NTSTATUS ultimap_waitForData(ULONG timeout, PULTIMAPDATAEVENT data)
/*
Called from usermode to wait for data
*/
{
	NTSTATUS r = STATUS_UNSUCCESSFUL;
	LARGE_INTEGER wait;
	PKWAIT_BLOCK waitblock = NULL;
	BOOL waiterRegistered = FALSE;

	if ((DataBlock == NULL) || UltimapStopping)
		return STATUS_UNSUCCESSFUL;

	ExAcquireFastMutex(&DataBlockMutex);
	if ((DataBlock != NULL) && (!UltimapStopping))
	{
		if (InterlockedIncrement(&UltimapWaiterCount) == 1)
			KeClearEvent(&UltimapWaitersDrained);
		waiterRegistered = TRUE;
	}
	ExReleaseFastMutex(&DataBlockMutex);

	if (!waiterRegistered)
		return STATUS_UNSUCCESSFUL;

	__try
	{
		waitblock=ExAllocatePool(NonPagedPool, MaxDataBlocks*sizeof(KWAIT_BLOCK));
		if (waitblock == NULL)
			__leave;

		wait.QuadPart=-10000LL * timeout;

		if (timeout==0xffffffff) //infinite wait
			r=KeWaitForMultipleObjects(MaxDataBlocks, DataReadyPointerList, WaitAny, UserRequest, UserMode, TRUE, NULL, waitblock);
		else
			r=KeWaitForMultipleObjects(MaxDataBlocks, DataReadyPointerList, WaitAny, UserRequest, UserMode, TRUE, &wait, waitblock);

		ExFreePool(waitblock);
		waitblock = NULL;

		data->Block=r-STATUS_WAIT_0;
		if (data->Block >= (UINT64)MaxDataBlocks)
		{
			r = STATUS_UNSUCCESSFUL;
			__leave;
		}

		ExAcquireFastMutex(&DataBlockMutex);
		__try
		{
			if ((DataBlock != NULL) && (!UltimapStopping))
			{
				data->KernelAddress=(UINT64)DataBlock[data->Block].Data;
				data->Mdl=(UINT64)IoAllocateMdl(DataBlock[data->Block].Data, DataBlock[data->Block].DataSize, FALSE, FALSE, NULL);
				if (data->Mdl)
				{
					MmBuildMdlForNonPagedPool((PMDL)(UINT_PTR)data->Mdl);
					data->Address=(UINT_PTR)MmMapLockedPagesSpecifyCache((PMDL)(UINT_PTR)data->Mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
					if (data->Address)
					{
						data->Size=DataBlock[data->Block].DataSize;
						data->CpuID=DataBlock[data->Block].CpuID;
						r=STATUS_SUCCESS;
					}
					else
						r=STATUS_UNSUCCESSFUL;
				}
				else
					r=STATUS_UNSUCCESSFUL;
			}
			else
				r=STATUS_UNSUCCESSFUL;
		}
		__finally
		{
			ExReleaseFastMutex(&DataBlockMutex);
		}
	}
	__finally
	{
		if (waitblock)
			ExFreePool(waitblock);

		if (InterlockedDecrement(&UltimapWaiterCount) == 0)
			KeSetEvent(&UltimapWaitersDrained, 0, FALSE);
	}

	return r;
}

/*
void apic_clearPerfmon()
{
	APIC_BASE->LVT_Performance_Monitor.a = APIC_BASE->LVT_Performance_Monitor.a & 0xff;
	APIC_BASE->EOI.a = 0;
}
*/
void ultimap_cleanstate()
{
	apic_clearPerfmon();
}

int perfmon_interrupt_centry(void)
{
	int currentCpu = cpunr();
	KIRQL old = PASSIVE_LEVEL;
	KIRQL entryIrql = KeGetCurrentIrql();
	int changedIRQL = 0;
	
	void *temp;
	int causedbyme;
	UINT_PTR blocksize;

	if (DS_AREA[currentCpu] == NULL)
	{
		disableInterrupts();
		return FALSE;
	}

	causedbyme = (DS_AREA[currentCpu]->BTS_IndexBaseAddress >= DS_AREA[currentCpu]->BTS_InterruptThresholdAddress);

	DbgPrint("perfmon_interrupt_centry\n");


	if (causedbyme)
		ultimap_cleanstate();	

	/* Waiting, fast mutexes and file I/O below require PASSIVE_LEVEL. */
	if (entryIrql != PASSIVE_LEVEL)
	{
		DS_AREA[currentCpu]->BTS_IndexBaseAddress = DS_AREA[currentCpu]->BTS_BufferBaseAddress;
		disableInterrupts();
		return causedbyme;
	}

	blocksize=(UINT_PTR)(DS_AREA[currentCpu]->BTS_IndexBaseAddress-DS_AREA[currentCpu]->BTS_BufferBaseAddress);
	
	{	
		if (KeGetCurrentIrql() < DISPATCH_LEVEL)
		{
			//When called by the pre-emptive caller
			changedIRQL = 1;
			old = KeRaiseIrqlToDpcLevel();
		}


		DbgPrint("Entry cpunr=%d\n", currentCpu);
		DbgPrint("Entry threadid=%d\n", PsGetCurrentThreadId());
		

		temp=ExAllocatePool(NonPagedPool, blocksize);
		if (temp)
		{
			RtlCopyMemory(temp, (PVOID *)(UINT_PTR)DS_AREA[currentCpu]->BTS_BufferBaseAddress, blocksize);

			DbgPrint("temp=%p\n", temp);


			DS_AREA[currentCpu]->BTS_IndexBaseAddress=DS_AREA[currentCpu]->BTS_BufferBaseAddress; //don't reset on alloc error
		}
		else
		{
			DbgPrint("ExAllocatePool has failed\n");
			if (changedIRQL)
				KeLowerIrql(old);
			disableInterrupts();
			return causedbyme;
		}
		
		if (changedIRQL)
			KeLowerIrql(old);
		//should be passive mode, taskswitches and cpu switches will happen now (When this returns, I may not be on the same interrupt as I was when I started)


		if (SaveToFile)
		{
			IO_STATUS_BLOCK iosb;
			NTSTATUS r;

			//Instead of sending the data to a usermode app it was chosen to store the data to a file for later usage
			DbgPrint("Writing buffer to disk\n");			
			r=ZwWriteFile(FileHandle, NULL, NULL, NULL, &iosb,  temp, (ULONG)blocksize, NULL, NULL); 
			DbgPrint("Done Writing. Result=%x\n", r);
			ExFreePool(temp);
			temp = NULL;
		}
		else
		{
			DbgPrint("Waiting till there is a block free\n");
			//When all workers are busy do not continue
			if ((DataBlock) && (KeWaitForSingleObject(&DataBlockSemaphore, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS))
			{
				int currentblock;
				int i;

				//Enter a critical section and choose a block
				DbgPrint("Acquired semaphore. Now picking a usable datablock\n");

				
				ExAcquireFastMutex(&DataBlockMutex);
				DbgPrint("Acquired mutex. Looking for a Datablock that can be used\n");

				if (DataBlock)
				{
					currentblock=-1;
					for (i=0; i< MaxDataBlocks; i++)
					{
						if (DataBlock[i].Available) //look for a block that is set as available
						{
							currentblock=i;
							DataBlock[currentblock].Available=FALSE; //not available anymore
							break;
						}
					}
				}
				else currentblock=-1;



				if (currentblock>=0) 
				{					
					DbgPrint("Using datablock %d\n", currentblock);
					DataBlock[currentblock].Data=temp;
					DataBlock[currentblock].DataSize=(int)blocksize;
					DataBlock[currentblock].CpuID=currentCpu;
					
					DbgPrint("Calling KeSetEvent/KeWaitForSingleObject\n");
					KeSetEvent(&DataBlock[currentblock].DataReady, 1, FALSE); //Trigger a worker thread to start working					
				}	
				ExReleaseFastMutex(&DataBlockMutex);
				//DbgPrint("Released mutex\n");
				


			}
			else
			{
				DbgPrint("if ((DataBlock) && (KeWaitForSingleObject(&DataBlockSemaphore, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS)) failed\n");
			}
			
		}


		


		//and return to the caller process
		disableInterrupts();
		return causedbyme;

	}	
}


#ifdef AMD64
extern void perfmon_interrupt();
#else
_declspec( naked ) void perfmon_interrupt( void )
{
	__asm{
		cld

		push ebp
		mov ebp,esp

		//store state
		pushad
		xor eax,eax
		mov ax,ds
		push eax

		mov ax,es
		push eax

		mov ax,fs
		push eax

		mov ax,gs
		push eax

		mov ax,0x23 //0x10 should work too, but even windows itself is using 0x23
		mov ds,ax
		mov es,ax
		mov gs,ax
		mov ax,0x30
		mov fs,ax

		call perfmon_interrupt_centry
		cmp eax,1	//set flag


		//restore state
		pop gs
		pop fs
		pop es
		pop ds
		popad

		pop ebp		

		je skip_original_perfmon

		jmp far [perfmonJumpBackLocation]

skip_original_perfmon:
		// commented out: I don't think a APIC interrupt has an errorcode....  add esp,4 //undo errorcode push
		iretd
	}
}
#endif

VOID ultimap_pause_dpc(IN struct _KDPC *Dpc, IN PVOID DeferredContext, IN PVOID SystemArgumen1, IN PVOID SystemArgument2)
{
	vmx_ultimap_pause();
}

void ultimap_pause(void)
{
	forEachCpu(ultimap_pause_dpc, NULL, NULL, NULL, NULL);
}

VOID ultimap_resume_dpc(IN struct _KDPC *Dpc, IN PVOID DeferredContext, IN PVOID SystemArgumen1, IN PVOID SystemArgument2)
{
	vmx_ultimap_resume();
}

void ultimap_resume(void)
{
	forEachCpu(ultimap_resume_dpc, NULL, NULL, NULL, NULL);
}

VOID ultimap_disable_dpc(IN struct _KDPC *Dpc, IN PVOID DeferredContext, IN PVOID SystemArgumen1, IN PVOID SystemArgument2)
{
	//DbgPrint("ultimap_disable_dpc()\n");

	if (vmxusable)
		vmx_ultimap_disable();

	if (DS_AREA[cpunr()])
	{
		ExFreePool(DS_AREA[cpunr()]);
		DS_AREA[cpunr()]=NULL;
	}
}

void ultimap_disable_passive(UINT_PTR param)
{
	int currentCpu = cpunr();

	if (vmxusable)
		vmx_ultimap_disable();

	if ((currentCpu >= 0) && (currentCpu < 256) && DS_AREA[currentCpu])
	{
		ExFreePool(DS_AREA[currentCpu]);
		DS_AREA[currentCpu] = NULL;
	}
}

static void ultimap_unregister_hook(void)
{
	void *clear = NULL;

	if (UltimapHookRegistered)
	{
		NTSTATUS status = HalSetSystemInformation(HalProfileSourceInterruptHandler, sizeof(PVOID*), &clear);
		if (NT_SUCCESS(status))
			UltimapHookRegistered = FALSE;
		else
			DbgPrint("Failed to unregister Ultimap PMI hook: %x\n", status);
	}
}

static void ultimap_rollback_setup(BOOLEAN processorsConfigured)
{
	int i;

	UltimapStopping = TRUE;
	if (processorsConfigured)
		forEachCpuPassive(ultimap_disable_passive, 0);

	ultimap_unregister_hook();

	if (FileHandle)
	{
		ZwClose(FileHandle);
		FileHandle = NULL;
	}

	if (DataBlock)
	{
		for (i = 0; i < MaxDataBlocks; i++)
		{
			if (DataBlock[i].Data)
			{
				ExFreePool(DataBlock[i].Data);
				DataBlock[i].Data = NULL;
			}
		}
		ExFreePool(DataBlock);
		DataBlock = NULL;
	}

	if (DataReadyPointerList)
	{
		ExFreePool(DataReadyPointerList);
		DataReadyPointerList = NULL;
	}

	SaveToFile = FALSE;
	MaxDataBlocks = 1;
}



void ultimap_disable(void)
{
	if (DataBlock)
	{
		int i;

		if (!forEachCpu(ultimap_disable_dpc, NULL, NULL, NULL, NULL))
			return;

		if (SaveToFile && FileHandle) 
		{		
			ZwClose(FileHandle);
			FileHandle=NULL;
		}

		//all logging should have stopped now
		
		//Trigger all events waking up each thread that was waiting for the events

		ExAcquireFastMutex(&DataBlockMutex);

		for (i=0; i<MaxDataBlocks; i++)
			KeSetEvent(&DataBlock[i].DataReady,0, FALSE);

		UltimapStopping = TRUE;
		ExReleaseFastMutex(&DataBlockMutex);

		KeWaitForSingleObject(&UltimapWaitersDrained, Executive, KernelMode, FALSE, NULL);

		ExAcquireFastMutex(&DataBlockMutex);

		ExFreePool(DataBlock);
		DataBlock=NULL;

		if (DataReadyPointerList)
		{
			ExFreePool(DataReadyPointerList);
			DataReadyPointerList=NULL;		
		}
		ExReleaseFastMutex(&DataBlockMutex);

		ultimap_unregister_hook();


	}
}


VOID ultimap_setup_dpc(IN struct _KDPC *Dpc, IN PVOID  DeferredContext, IN PVOID  SystemArgument1, IN PVOID  SystemArgument2)
/*
initializes ultimap. If the DS_AREA_SIZE is bigger than 0 it will allocate the required region (the usual option, but if not used it could be a LBR only thing)
Call this for each processor
*/
{
	struct
	{
		UINT64 cr3;
		UINT64 dbgctl_msr;
		int DS_AREA_SIZE;
		volatile LONG SetupFailed;
	} *params;
	int currentCpu;

	params=DeferredContext;
	currentCpu = cpunr();

	DS_AREA_SIZE=params->DS_AREA_SIZE;
	if ((DS_AREA_SIZE == 0) || (currentCpu < 0) || (currentCpu >= 256))
	{
		DbgPrint("Invalid Ultimap CPU or DS area size\n");
		InterlockedExchange(&params->SetupFailed, 1);
		return;
	}
	

	DbgPrint("ultimap(%I64x, %I64x, %d)", (UINT64)params->cr3, (UINT64)params->dbgctl_msr, params->DS_AREA_SIZE);
	DS_AREA[currentCpu]=NULL;

	if (params->DS_AREA_SIZE)
	{
		DS_AREA[currentCpu]=ExAllocatePool(NonPagedPool, params->DS_AREA_SIZE);

		if (DS_AREA[currentCpu] == NULL)
		{
			DbgPrint("ExAllocatePool failed\n");
			InterlockedExchange(&params->SetupFailed, 1);
			return;
		}

		RtlZeroMemory(DS_AREA[currentCpu],  params->DS_AREA_SIZE);

		DbgPrint("DS_AREA[%d]=%p", currentCpu, DS_AREA[currentCpu]);

		//Initialize the DS_AREA 

		DS_AREA[currentCpu]->BTS_BufferBaseAddress=(QWORD)(UINT_PTR)DS_AREA[currentCpu]+sizeof(DS_AREA_MANAGEMENT);
        DS_AREA[currentCpu]->BTS_BufferBaseAddress+=sizeof(BTS);

        DS_AREA[currentCpu]->BTS_BufferBaseAddress-=DS_AREA[currentCpu]->BTS_BufferBaseAddress % sizeof(BTS);

        DS_AREA[currentCpu]->BTS_IndexBaseAddress=DS_AREA[currentCpu]->BTS_BufferBaseAddress;
        DS_AREA[currentCpu]->BTS_AbsoluteMaxAddress=(QWORD)(UINT_PTR)DS_AREA[currentCpu]+params->DS_AREA_SIZE-sizeof(BTS);
        DS_AREA[currentCpu]->BTS_AbsoluteMaxAddress-=DS_AREA[currentCpu]->BTS_AbsoluteMaxAddress % sizeof(BTS);
        DS_AREA[currentCpu]->BTS_AbsoluteMaxAddress++;

		DS_AREA[currentCpu]->BTS_InterruptThresholdAddress=(DS_AREA[currentCpu]->BTS_AbsoluteMaxAddress-1) - 4*sizeof(BTS);
	}
	

	if (params->dbgctl_msr & (1 << 8))
	{
		//hook the perfmon interrupt. First get the interrupt assigned (usually 0xfe, but let's be sure and read it from the apic)		

		int perfmonIVT=(APIC_BASE->LVT_Performance_Monitor.a) & 0xff;

		DbgPrint("APIC_BASE->LVT_Performance_Monitor.a=%x\n", APIC_BASE->LVT_Performance_Monitor.a);
		if (perfmonIVT==0) //if not setup at all then set it up now
			perfmonIVT=0xfe; 

		APIC_BASE->LVT_Performance_Monitor.a=perfmonIVT; //clear mask flag if it was set

		DbgPrint("APIC_BASE->LVT_Performance_Monitor.a=%x\n", APIC_BASE->LVT_Performance_Monitor.a);

	
		/*

		if (inthook_HookInterrupt((unsigned char)perfmonIVT, getCS(), (ULONG_PTR)perfmon_interrupt, &perfmonJumpBackLocation))
			DbgPrint("Interrupt hooked\n");
		else
			DbgPrint("Failed to hook interrupt\n");
			*/

	}

	//and finally activate the mapping
	if (vmxusable)
	{
		vmx_ultimap((UINT_PTR)params->cr3, params->dbgctl_msr, DS_AREA[currentCpu]);
	}
	else
	{
		DbgPrint("vmxusable is false. So no ultimap for you!!!\n");
		InterlockedExchange(&params->SetupFailed, 1);
	}
}

void ultimapapc(PKAPC Apc, PKNORMAL_ROUTINE NormalRoutine, PVOID NormalContext, PVOID SystemArgument1, PVOID SystemArgument2)
{
	EFLAGS e = getEflags();
	DbgPrint("ultimapapc call for cpu %d ( IF=%d IRQL=%d)\n", KeGetCurrentProcessorNumber(), e.IF, KeGetCurrentIrql());
	DbgPrint("SystemArgument1=%x\n", *(PULONG)SystemArgument1);
	DbgPrint("tid=%x\n", PsGetCurrentThreadId());
	DbgPrint("Apc=%p\n", Apc);
}

void ultimapapcnormal(PVOID arg1, PVOID arg2, PVOID arg3)
{
	EFLAGS e = getEflags();
	DbgPrint("ultimapapcnormal call for cpu %d ( IF=%d IRQL=%d)\n", KeGetCurrentProcessorNumber(), e.IF, KeGetCurrentIrql());
	DbgPrint("tid=%x\n", PsGetCurrentThreadId());

	ultimap_flushBuffers();

	return;
}

KAPC      kApc[128];
volatile LONG apcnr = 0;

void perfmon_hook(__in struct _KINTERRUPT *Interrupt, __in PVOID ServiceContext)
{	
	
	int i = InterlockedIncrement(&apcnr) % 128;

	
	EFLAGS e = getEflags();
	DbgPrint("permon_hook call for cpu %d ( IF=%d IRQL=%d)\n", KeGetCurrentProcessorNumber(), e.IF, KeGetCurrentIrql());

	DbgPrint("kApc=%p\n", &kApc);


	//switch buffer pointers

	//call DPC for ultimap for this cpu


	//todo: if this is buggy use a dpc instead to create the apc.  (slower)
	KeInitializeApc(&kApc[i],
		(PKTHREAD)PsGetCurrentThread(), 
		0,
		(PKKERNEL_ROUTINE)ultimapapc,
		NULL,
		(PKNORMAL_ROUTINE)ultimapapcnormal,
		KernelMode,
		0
		);

	KeInsertQueueApc(&kApc[i], NULL, NULL, 0);

	

	DbgPrint("after KeInsertQueueApc");




	//perfmon_interrupt_centry();
	ultimap_cleanstate();

	DbgPrint("permon_return");
}

void *pperfmon_hook = (void*)perfmon_hook;

NTSTATUS ultimap(UINT64 cr3, UINT64 dbgctl_msr, int _DS_AREA_SIZE, BOOL savetofile, WCHAR *filename, int handlerCount)
{
	struct
	{
		UINT64 cr3;
		UINT64 dbgctl_msr;
		int DS_AREA_SIZE;
		volatile LONG SetupFailed;
	} params;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	BOOLEAN processorsConfigured = FALSE;
	int i;

	if ((handlerCount <= 0) || (handlerCount > 64) ||
		(_DS_AREA_SIZE < (int)(sizeof(DS_AREA_MANAGEMENT) + 6 * sizeof(BTS))) ||
		(savetofile && (filename == NULL)))
		return STATUS_INVALID_PARAMETER;

	if (DataBlock || DataReadyPointerList || FileHandle || UltimapHookRegistered || !UltimapStopping)
		return STATUS_DEVICE_BUSY;

	for (i = 0; i < 256; i++)
	{
		if (DS_AREA[i])
			return STATUS_DEVICE_BUSY;
	}

	MaxDataBlocks = handlerCount;
	KeInitializeSemaphore(&DataBlockSemaphore, MaxDataBlocks, MaxDataBlocks);
	ExInitializeFastMutex(&DataBlockMutex);
	KeInitializeEvent(&UltimapWaitersDrained, NotificationEvent, TRUE);
	UltimapWaiterCount = 0;
	UltimapStopping = TRUE;
	FileHandle = NULL;
	SaveToFile = savetofile;

	if (SaveToFile)
	{
		UNICODE_STRING usFile;
		OBJECT_ATTRIBUTES oaFile;
		IO_STATUS_BLOCK iosb;

		RtlInitUnicodeString(&usFile, filename);
		InitializeObjectAttributes(&oaFile,&usFile, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,NULL);

		DbgPrint("Creating file %S", usFile.Buffer);

		status=ZwCreateFile(&FileHandle,SYNCHRONIZE|FILE_READ_DATA|FILE_APPEND_DATA | GENERIC_ALL,&oaFile,&iosb,0,FILE_ATTRIBUTE_NORMAL,0,FILE_SUPERSEDE, FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
		DbgPrint("ZwCreateFile=%x\n", status);
		if (!NT_SUCCESS(status))
			goto setupFailed;
	}

	//Datablock inits
	DataBlock=ExAllocatePool(NonPagedPool, sizeof(_DataBlock) * MaxDataBlocks);
	DataReadyPointerList=ExAllocatePool(NonPagedPool, sizeof(PVOID) * MaxDataBlocks);
	if (DataBlock)
		RtlZeroMemory(DataBlock, sizeof(_DataBlock) * MaxDataBlocks);
	if (DataReadyPointerList)
		RtlZeroMemory(DataReadyPointerList, sizeof(PVOID) * MaxDataBlocks);

	if ((DataBlock == NULL) || (DataReadyPointerList == NULL))
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto setupFailed;
	}

	for (i=0; i< MaxDataBlocks; i++)
	{
		KeInitializeEvent(&DataBlock[i].DataReady, SynchronizationEvent , FALSE);
		DataBlock[i].Available=TRUE;
		DataReadyPointerList[i]=&DataBlock[i].DataReady;
	}

	params.cr3=cr3;
	params.dbgctl_msr=dbgctl_msr;
	params.DS_AREA_SIZE=_DS_AREA_SIZE;
	params.SetupFailed = 0;

	status=HalSetSystemInformation(HalProfileSourceInterruptHandler, sizeof(PVOID*), &pperfmon_hook); //hook the perfmon interrupt
	DbgPrint("HalSetSystemInformation returned %x\n", status);
	if (!NT_SUCCESS(status))
		goto setupFailed;
	UltimapHookRegistered = TRUE;

	if (!forEachCpu(ultimap_setup_dpc, &params, NULL, NULL, NULL))
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto setupFailed;
	}
	processorsConfigured = TRUE;
	if (params.SetupFailed)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto setupFailed;
	}

	UltimapStopping = FALSE;
	return STATUS_SUCCESS;

setupFailed:
	ultimap_rollback_setup(processorsConfigured);
	return status;
}
