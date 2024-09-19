
#include <ntddk.h>

#include <wdm.h>
#include<ntstrsafe.h>

#include "nvme.h"

//ALL_DEVICE_INFO g_AllDevInfo;

#define arraysize( p ) ( sizeof( p ) / sizeof( ( p )[0] ) )

#define DEVICE_NAME			( L"\\Device\\WINNVME" )
#define DEVICE_SYMLINKNAME	( L"\\DosDevices\\WINNVME" )

typedef struct _DEVICE_EXTENSION
{
	PDEVICE_OBJECT		fdo;
	PDEVICE_OBJECT		PhyDevice;
	PDEVICE_OBJECT		NextStackDevice;
	UNICODE_STRING	ustrDeviceName;    
	UNICODE_STRING	ustrSymLinkName;  
	PVOID							InterruptObject;

	BOOLEAN					bInterruptEnable;

	PVOID							bar0;
	SIZE_T							bar_size;

	PVOID							admin_sq_pvk;
	PVOID							admin_cq_pvk;

	PVOID							data_buffer;

	ULONG						vector;

	u32*							admin_sq_doorbell;
	u32*							admin_cq_doorbell;
	int								admin_sq_tail ;
	int								admin_cq_head ;
	int								admin_cq_phase;

	int								admin_sq_size;       ///< queue size
	int								admin_cq_size;       ///< queue size

	//nvme_sq_entry_t*				sq;
	//nvme_cq_entry_t*				cq;

} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

void WinNVMeDelay(long long millsecond);

NTSTATUS WinNVMeAddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT PhysicalDeviceObject);
NTSTATUS WinNVMePnp(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WinNVMeDeviceControl(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
NTSTATUS WinNVMeDispatchRoutine(IN PDEVICE_OBJECT fdo, IN PIRP Irp);
void WinNVMeUnload(IN PDRIVER_OBJECT DriverObject);

NTSTATUS ReadWriteConfigSpace(IN PDEVICE_OBJECT DeviceObject, IN ULONG ReadOrWrite, // 0 for read 1 for write
													IN PVOID Buffer, IN ULONG Offset, IN ULONG Length );

extern "C"
NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverObject, IN PUNICODE_STRING pRegistryPath)
{

	UNREFERENCED_PARAMETER(pRegistryPath);

	pDriverObject->DriverExtension->AddDevice = WinNVMeAddDevice;
	pDriverObject->MajorFunction[IRP_MJ_PNP] = WinNVMePnp;
	pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinNVMeDeviceControl;
	pDriverObject->MajorFunction[IRP_MJ_CREATE] = WinNVMeDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_CLOSE] = WinNVMeDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_READ] = WinNVMeDispatchRoutine;
	pDriverObject->MajorFunction[IRP_MJ_WRITE] = WinNVMeDispatchRoutine;
	pDriverObject->DriverUnload = WinNVMeUnload;

	return STATUS_SUCCESS;
}

UINT8 gucDeviceCounter;


BOOLEAN
MSI_ISR(
	IN  PKINTERRUPT  Interrupt,
	PVOID  ServiceContext,
	ULONG  MessageId
)
{
	UNREFERENCED_PARAMETER(Interrupt);


	DbgPrint("Interrupt Occured: %d", MessageId);

	PDEVICE_EXTENSION p = (PDEVICE_EXTENSION)ServiceContext;

	nvme_cq_entry_t * admin_cq = (nvme_cq_entry_t*)p->admin_cq_pvk;
	
	if (MessageId == 0) {
		if (admin_cq[p->admin_cq_head].u.a.p == p->admin_cq_phase) {
			while (admin_cq[p->admin_cq_head].u.a.p == p->admin_cq_phase) {
				//int head = p->admin_cq_head;
				if (++p->admin_cq_head == p->admin_cq_size) {
					p->admin_cq_head = 0;
					p->admin_cq_phase = !p->admin_cq_phase;
				}

				*(volatile u32*)(p->admin_cq_doorbell) = p->admin_cq_head;
			}
		}
	}
	else {
	
	
	
	
	
	}


	return TRUE;
}

BOOLEAN
FdoInterruptCallback(
	IN  PKINTERRUPT             InterruptObject,
	IN  PVOID                   Context
)
{
	UNREFERENCED_PARAMETER(InterruptObject);
	UNREFERENCED_PARAMETER(Context);
	
	return TRUE;
}


NTSTATUS WinNVMeAddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS			status;
	PDEVICE_OBJECT		fdo;
	PDEVICE_EXTENSION	pdx;
	UNICODE_STRING		devName;
	UNICODE_STRING		symLinkName;

	//DECLARE_UNICODE_STRING_SIZE(devName, 64);
	//DECLARE_UNICODE_STRING_SIZE(symLinkName, 64);
	
	wchar_t  devNameReal[64] = { 0 };
	wchar_t  symLinkNameReal[64] = { 0 };
	swprintf(devNameReal, L"%s%d", DEVICE_NAME, gucDeviceCounter);
	DbgPrint("%ls\n",devNameReal);
	RtlInitUnicodeString(&devName, devNameReal);
	//RtlUnicodeStringPrintf(&devName, L"\\Device\\WINMEM%d", gucDeviceCounter);

	DbgPrint("Add Device");
	status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoCreateDevice");
		return status;
	}
	pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;
	pdx->fdo = fdo;
	pdx->PhyDevice = PhysicalDeviceObject;
	//pdx->NextStackDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
	status = IoAttachDeviceToDeviceStackSafe(fdo, PhysicalDeviceObject, &pdx->NextStackDevice);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoAttachDeviceToDeviceStackSafe");
		return status;
	}

	//RtlUnicodeStringPrintf(&symLinkName, L"\\DosDevices\\WINMEM_%d", gucDeviceCounter);

	swprintf(symLinkNameReal, L"%s_%d", DEVICE_SYMLINKNAME, gucDeviceCounter);
	DbgPrint("%ls\n", symLinkNameReal);
	RtlInitUnicodeString(&symLinkName, symLinkNameReal);
	//RtlInitUnicodeString(&symLinkName, DEVICE_SYMLINKNAME);

	pdx->admin_sq_tail = 0;
	pdx->admin_cq_head = 0;
	pdx->admin_cq_phase = 1;

	pdx->admin_sq_doorbell = nullptr;
	pdx->admin_cq_doorbell = nullptr;

	//pdx->sq = nullptr;
	//pdx->cq = nullptr;

	pdx->data_buffer = nullptr;

	pdx->admin_cq_pvk = nullptr;
	pdx->admin_sq_pvk = nullptr;

	pdx->bar0 = nullptr;

	pdx->bInterruptEnable = FALSE;
	pdx->ustrDeviceName = devName;
	pdx->ustrSymLinkName = symLinkName;
	status = IoCreateSymbolicLink(&symLinkName, &devName); 

	if (!NT_SUCCESS(status))
	{
		DbgPrint("Failure IoCreateSymbolicLink");

		IoDeleteSymbolicLink(&pdx->ustrSymLinkName);
		status = IoCreateSymbolicLink(&symLinkName, &devName);
		if (!NT_SUCCESS(status))
		{
			DbgPrint("Failure IoCreateSymbolicLink");
			return status;
		}
	}

	fdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
	fdo->Flags &= ~DO_DEVICE_INITIALIZING;

	DbgPrint("Success DriverEntry");

	gucDeviceCounter++;
	return STATUS_SUCCESS;
}



NTSTATUS DefaultPnpHandler(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(pdx->NextStackDevice, Irp);
}




NTSTATUS OnRequestComplete(PDEVICE_OBJECT junk, PIRP Irp, PKEVENT pev)
{
	UNREFERENCED_PARAMETER(junk);
	UNREFERENCED_PARAMETER(Irp);

	KeSetEvent(pev, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS ForwardAndWait(PDEVICE_EXTENSION pdx, PIRP Irp)
{       
	KEVENT event;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, (PIO_COMPLETION_ROUTINE)OnRequestComplete, (PVOID) & event, TRUE, TRUE, TRUE);

	IoCallDriver(pdx->NextStackDevice, Irp);
	KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	return Irp->IoStatus.Status;
}                                                       


VOID ShowResources(IN PCM_PARTIAL_RESOURCE_LIST list, IN PDEVICE_EXTENSION pdx)
{                                                       
	ULONG							i;
	ULONG							nres = list->Count; 
	//NTSTATUS			status;

	PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = list->PartialDescriptors;

	//IO_CONNECT_INTERRUPT_PARAMETERS     Connect;
	//IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

	UNREFERENCED_PARAMETER(pdx);
	//PIO_INTERRUPT_MESSAGE_INFO  p;
	//PIO_INTERRUPT_MESSAGE_INFO_ENTRY pp;
	//NTSTATUS status;

	for (i = 0; i < nres; ++i, ++resource)
	{                                                   
		ULONG		type = resource->Type;

		static char* name[] = {
			"CmResourceTypeNull",
			"CmResourceTypePort",
			"CmResourceTypeInterrupt",
			"CmResourceTypeMemory",
			"CmResourceTypeDma",
			"CmResourceTypeDeviceSpecific",
			"CmResourceTypeBusNumber",
			"CmResourceTypeDevicePrivate",
			"CmResourceTypeAssignedResource",
			"CmResourceTypeSubAllocateFrom",
		};

		DbgPrint("type=%d, typeName=%s \n", type, type < arraysize(name) ? name[type] : "unknown");

		switch (type)
		{   // select on resource type
		case CmResourceTypePort:
		case CmResourceTypeMemory:
			pdx->bar_size = resource->u.Port.Length;
			pdx ->bar0 = MmMapIoSpace(resource->u.Port.Start, resource->u.Port.Length, MmNonCached);
			//DbgPrint("bar0  kernel virtual address:  %p", pdx->bar0);

			//DbgPrint("CmResourceTypeMemory ===> start 0x%lX 0x%lX length:%d\n",
			//	resource->u.Port.Start.HighPart,
			//	resource->u.Port.Start.LowPart,
			//	resource->u.Port.Length);
			break;
		case CmResourceTypeBusNumber:
			//DbgPrint("CmResourceTypeBusNumber:::");
			break;
		case CmResourceTypeInterrupt:

#if  0
			RtlZeroMemory(&Connect, sizeof(IO_CONNECT_INTERRUPT_PARAMETERS));
			RtlZeroMemory(&Disconnect, sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS));

#if 1
			Connect.Version = CONNECT_FULLY_SPECIFIED;
			Connect.FullySpecified.PhysicalDeviceObject = pdx->PhyDevice;
		
			Connect.FullySpecified.InterruptObject = &pdx->InterruptObject;
			Connect.FullySpecified.ServiceRoutine = FdoInterruptCallback;
			Connect.FullySpecified.ServiceContext = pdx->fdo;

			Connect.FullySpecified.FloatingSave = FALSE;
			Connect.FullySpecified.SpinLock = NULL;


			if (resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
				Connect.FullySpecified.Vector = resource->u.MessageInterrupt.Translated.Vector;
				Connect.FullySpecified.Irql = (KIRQL)resource->u.MessageInterrupt.Translated.Level;
				Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.MessageInterrupt.Translated.Level;
				Connect.FullySpecified.Group = resource->u.MessageInterrupt.Translated.Group;
				Connect.FullySpecified.ProcessorEnableMask = resource->u.MessageInterrupt.Translated.Affinity;
			}
			else {
				Connect.FullySpecified.Vector = resource->u.Interrupt.Vector;
				Connect.FullySpecified.Irql = (KIRQL)resource->u.Interrupt.Level;
				Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.Interrupt.Level;
		     	Connect.FullySpecified.Group = resource->u.Interrupt.Group;
				Connect.FullySpecified.ProcessorEnableMask = resource->u.Interrupt.Affinity;
			}

			//Connect.Version = (Connect.FullySpecified.Group != 0) ? CONNECT_FULLY_SPECIFIED_GROUP : CONNECT_FULLY_SPECIFIED;
			Connect.FullySpecified.InterruptMode == (resource->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
			Connect.FullySpecified.ShareVector = (BOOLEAN)(resource->ShareDisposition == CmResourceShareShared);
			

			status = IoConnectInterruptEx(&Connect);

			if (NT_SUCCESS(status)) {
				DbgPrint("Success IoConnectInterruptEx");
				//p = (PIO_INTERRUPT_MESSAGE_INFO)pdx->InterruptObject;
				//pp = p->MessageInfo;
				//DbgPrint("interrupt version: %d", Connect.Version);

				//for (i = 0; i < p->MessageCount; ++i) {
				//	DbgPrint("IoConnectInterruptEx params ===> Irql:%X, Vector:%X, Proc:%llX, MessageData:%lX, MessageAddress:%lX\n",
				//		(pp + i)->Irql,
				//		(pp + i)->Vector,
				//		(pp + i)->TargetProcessorSet,
				//		(pp + i)->MessageData,
				//		(pp + i)->MessageAddress.LowPart
				//	);
				//}

				Disconnect.Version = Connect.Version;
				Disconnect.ConnectionContext.InterruptObject = pdx->InterruptObject;
				IoDisconnectInterruptEx(&Disconnect);
			}
			else {
				DbgPrint("Failure  IoConnectInterruptEx:   %x", status);

			}

#endif

			//}
			//else {
			//	Connect.FullySpecified.Vector = resource->u.Interrupt.Vector;
			//	Connect.FullySpecified.Irql = (KIRQL)resource->u.Interrupt.Level;
			//	Connect.FullySpecified.SynchronizeIrql = (KIRQL)resource->u.Interrupt.Level;
			//	Connect.FullySpecified.Group = resource->u.Interrupt.Group;
			//	Connect.FullySpecified.ProcessorEnableMask = resource->u.Interrupt.Affinity;
			
			//}




			//if (resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) {
			//	DbgPrint("CM_RESOURCE_INTERRUPT_MESSAGE\n");
			//}


			DbgPrint("resource flag: %x\n", resource->Flags);
			DbgPrint("CmResourceTypeInterrupt   Translated ===> level:%X, vector:%X, affinity:%llX\n",
				resource->u.MessageInterrupt.Translated.Level,
				resource->u.MessageInterrupt.Translated.Vector,
				resource->u.MessageInterrupt.Translated.Affinity);

			DbgPrint("CmResourceTypeInterrupt   Raw ===> level:%X, vector:%X, affinity:%llX\n",
				resource->u.MessageInterrupt.Translated.Level,
				resource->u.MessageInterrupt.Translated.Vector,
				resource->u.MessageInterrupt.Translated.Affinity);

#endif
			break;

		case CmResourceTypeDma:
			DbgPrint("CmResourceTypeDma ===> channel %d, port %X\n", resource->u.Dma.Channel, resource->u.Dma.Port);
		} // select on resource type

	}    // for each resource
}       // ShowResources






BOOLEAN OnInterrupt(PKINTERRUPT InterruptObject, PDEVICE_EXTENSION pdx)
{   
	UNREFERENCED_PARAMETER(InterruptObject);
	UNREFERENCED_PARAMETER(pdx);
	

	return TRUE;
}




NTSTATUS HandleStartDevice(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	NTSTATUS						status;
	PIO_STACK_LOCATION				stack;
	//PCM_PARTIAL_RESOURCE_LIST		raw;
	PCM_PARTIAL_RESOURCE_LIST		translated;
	PCM_FULL_RESOURCE_DESCRIPTOR	pfrd;

	IO_CONNECT_INTERRUPT_PARAMETERS     Connect;

	PIO_INTERRUPT_MESSAGE_INFO  p;
	PIO_INTERRUPT_MESSAGE_INFO_ENTRY pp;

	//PCI_COMMON_CONFIG				pci_config;
	//UINT8		buffer[256] = { 0 };
	UINT16     command_reg;

	UINT32 i = 0;

	status = ForwardAndWait(pdx, Irp);        
	

	if (!NT_SUCCESS(status))                
	{
		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;
	}


	stack = IoGetCurrentIrpStackLocation(Irp);

	// Check bus Master
	status = ReadWriteConfigSpace(pdx->fdo, 0, &command_reg, 4, 2);
	DbgPrint("Command Register: 0x%X", command_reg);


	if (stack->Parameters.StartDevice.AllocatedResourcesTranslated)
	{
		translated = &stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0].PartialResourceList;
		pfrd = &stack->Parameters.StartDevice.AllocatedResourcesTranslated->List[0];
	}
	else
	{
		translated = NULL;
	}
	
#if 0
	status = ReadWriteConfigSpace(pdx->fdo, 0, &buffer, 0, 256);
	UINT8 next = buffer[0x34];
	DbgPrint("Next Pointer: 0x%X", next);

#if 1
	while (1) {
		DbgPrint("Cap ID: 0x%X", buffer[next]);
		next = buffer[next + 1];

		if (++i > 10 || next == 0) break;
	}
#endif


	//for ( i = 0; i <100; i=i+16) {
	//	DbgPrint("%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x", 
	//		pci_config.DeviceSpecific[i + 0], pci_config.DeviceSpecific[i+1], pci_config.DeviceSpecific[i + 2], pci_config.DeviceSpecific[i + 3],
	//		pci_config.DeviceSpecific[i + 4], pci_config.DeviceSpecific[i + 5],pci_config.DeviceSpecific[i + 6],pci_config.DeviceSpecific[i + 7],
	//		pci_config.DeviceSpecific[i + 8], pci_config.DeviceSpecific[i + 9], pci_config.DeviceSpecific[i + 10], pci_config.DeviceSpecific[i + 11],
	//		pci_config.DeviceSpecific[i + 12], pci_config.DeviceSpecific[i + 13], pci_config.DeviceSpecific[i + 14], pci_config.DeviceSpecific[i + 15]
	//	);
	//}
#endif

	// Show resource from PNP Manager
	ShowResources( translated, pdx);

#if 1
	RtlZeroMemory(&Connect, sizeof(IO_CONNECT_INTERRUPT_PARAMETERS));
	Connect.Version = CONNECT_MESSAGE_BASED;
	Connect.MessageBased.ConnectionContext.Generic = &pdx->InterruptObject;
	Connect.MessageBased.PhysicalDeviceObject = pdx->PhyDevice;
	Connect.MessageBased.FloatingSave = FALSE;
	Connect.MessageBased.SpinLock = nullptr;
	Connect.MessageBased.MessageServiceRoutine = MSI_ISR;
	Connect.MessageBased.SynchronizeIrql = 0;
	Connect.MessageBased.ServiceContext = pdx;
	Connect.MessageBased.FallBackServiceRoutine = FdoInterruptCallback;

	status = IoConnectInterruptEx(&Connect);

	if (NT_SUCCESS(status)) {
		DbgPrint("Success IoConnectInterruptEx");
		pdx->bInterruptEnable = TRUE;
		p = (PIO_INTERRUPT_MESSAGE_INFO)pdx->InterruptObject;
		pp = p->MessageInfo;
		DbgPrint("interrupt version: %d", Connect.Version);

		for ( i = 0; i < p->MessageCount; ++i) {
			DbgPrint("IoConnectInterruptEx params ===> Irql:%X, Vector:%X, Proc:%llX, MessageData:%lX, MessageAddress:%lX\n",
				(pp + i)->Irql,
				(pp + i)->Vector,
				(pp + i)->TargetProcessorSet,
				(pp + i)->MessageData,
				(pp + i)->MessageAddress.LowPart
			);
		}		
	}

#endif

	// Check bus Master
	status = ReadWriteConfigSpace(pdx->fdo, 0, &command_reg, 4, 2);

	DbgPrint("Command Register: 0x%X", command_reg);


	nvme_controller_reg_t* ctrl_reg = (nvme_controller_reg_t*)(pdx ->bar0)  ;

	nvme_controller_cap_t cap	= { 0 };
	nvme_adminq_attr_t	aqa		= { 0 };
	nvme_controller_config_t cc = { 0 };

	cap.val = ctrl_reg->cap.val;

	// wait controller disable
	ctrl_reg->cc.a.en = 0;

	while (ctrl_reg->csts.rdy == 1) {
		DbgPrint("Waiting  controller disable");

		WinNVMeDelay(1);
	
	}
#if 1

	// Create Completion Queue
	PHYSICAL_ADDRESS pa = { 0x3, (LONG)0xffffffff };
	pdx -> admin_cq_pvk = MmAllocateContiguousMemory(sizeof(nvme_cq_entry_t) * 64, pa);

	PHYSICAL_ADDRESS cq_phyaddr = MmGetPhysicalAddress(pdx->admin_cq_pvk);
	DbgPrint("Admin CQ: %llX  ", cq_phyaddr.QuadPart);
	

	//  Create Submission Queue
	 //pa = { 0x3, (LONG)0xffffffff };
	 pdx -> admin_sq_pvk   = MmAllocateContiguousMemory(sizeof(nvme_sq_entry_t) * 64, pa);

	 PHYSICAL_ADDRESS sq_phyaddr = MmGetPhysicalAddress(pdx->admin_sq_pvk);
	 DbgPrint("Admin SQ: %llX  ", sq_phyaddr.QuadPart);


	
	nvme_cq_entry_t* cq = (nvme_cq_entry_t*)pdx->admin_cq_pvk;

	RtlZeroMemory(cq, sizeof(nvme_cq_entry_t) * 64);

	 pdx->admin_sq_size = 64;
	 pdx->admin_cq_size = 64;

	 aqa.a.asqs = 64 - 1;
	 aqa.a.acqs = 64 - 1;
	 ctrl_reg->aqa.val = aqa.val;

	 ctrl_reg->asq = sq_phyaddr.QuadPart;
	 ctrl_reg->acq = cq_phyaddr.QuadPart;

	 ctrl_reg->aqa.val = aqa.val;

	pdx ->admin_sq_doorbell = ctrl_reg->sq0tdbl;
	pdx ->admin_cq_doorbell = ctrl_reg->sq0tdbl + ((LONGLONG)1 << cap.a.dstrd);

	DbgPrint("sq dbl   :%p", pdx->admin_sq_doorbell);

	DbgPrint("cq dbl   :%p", pdx->admin_cq_doorbell);

	cc.val = NVME_CC_CSS_NVM;
	cc.val |= 0 << NVME_CC_MPS_SHIFT;
	cc.val |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	cc.val |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	cc.a.en = 1;

	ctrl_reg->cc.val = cc.val;

	while (ctrl_reg->csts.rdy == 0) {
		DbgPrint("Waiting  controller ready");

		WinNVMeDelay(1);

	}

	
	//Data Buffer
	pa = { 0x3, (LONG)0xffffffff };
	pdx->data_buffer = MmAllocateContiguousMemory(4096, pa);

	PHYSICAL_ADDRESS data = MmGetPhysicalAddress(pdx->data_buffer);
	DbgPrint("Data Buffer: %llX  ", data.QuadPart);

#if 1
	int cid = pdx->admin_sq_tail;

	nvme_sq_entry_t* sq = (nvme_sq_entry_t*)pdx->admin_sq_pvk;
	if (sq != nullptr) {
		sq[cid].get_log_page.opcode = nvme_admin_get_log_page;

		sq[cid].get_log_page.command_id = (u16)cid;
		sq[cid].get_log_page.nsid = 0xffffffff;
		sq[cid].get_log_page.dptr.prp1 = data.QuadPart;

		sq[cid].get_log_page.lid = 2;

		sq[cid].get_log_page.numdl = (4096 / sizeof(u32) - 1) & 0xff;
		sq[cid].get_log_page.numdu = ((4096 / sizeof(u32) - 1) >> 16) & 0xff;
	}
	

	if (++pdx->admin_sq_tail == pdx->admin_sq_size) pdx->admin_sq_tail = 0;

	*(volatile u32*)pdx->admin_sq_doorbell = pdx->admin_sq_tail;

#endif

#endif


#if 0
	PVOID pvu = nullptr;
	PHYSICAL_ADDRESS pa = { 0x3, (LONG)0xffffffff };
	PVOID pvk = MmAllocateContiguousMemory(16384, pa);
	PMDL pMdl = nullptr;

	if (pvk) {
		DbgPrint("Success  MmAllocateContiguousMemory\n");
		
		pMdl = IoAllocateMdl(pvk, 4096, FALSE, FALSE, NULL);
		if (pMdl) {
			DbgPrint("Success  IoAllocateMdl\n");

			MmBuildMdlForNonPagedPool(pMdl);
			pvu = MmMapLockedPagesSpecifyCache(pMdl, UserMode, MmNonCached, NULL, FALSE, NormalPagePriority);
			if (pvu) {
				DbgPrint("Success  MmMapLockedPagesSpecifyCache\n");
			}
			else {
				DbgPrint("Failure  MmMapLockedPagesSpecifyCache\n");
			}
		}
		else {
			DbgPrint("Failure  IoAllocateMdl\n");
		}
	}
	else {
		DbgPrint("Failure  MmAllocateContiguousMemory\n");

	}

	if(pvu  && pMdl ) MmUnmapLockedPages(pvu, pMdl);

	if(pMdl) IoFreeMdl(pMdl);

	if (pvk) {
		MmFreeContiguousMemory(pvk);
	}
#endif

	




	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}





NTSTATUS HandleRemoveDevice(PDEVICE_EXTENSION pdx, PIRP Irp)
{
	NTSTATUS status;
	IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;



	Irp->IoStatus.Status = STATUS_SUCCESS;
	status = DefaultPnpHandler(pdx, Irp);

	if (pdx->data_buffer) {
		DbgPrint("delete data_buffer");
		MmFreeContiguousMemory(pdx->data_buffer);
	}


	if (pdx->admin_cq_pvk) {
		DbgPrint("delete admin compeletion queue");
		MmFreeContiguousMemory(pdx->admin_cq_pvk);
	}

	if (pdx->admin_sq_pvk) {
		DbgPrint("delete admin submission queue");
		MmFreeContiguousMemory(pdx->admin_sq_pvk);
	}
	
	if (pdx -> bInterruptEnable) {
		DbgPrint("IoDisconnectInterruptEx");

		RtlZeroMemory(&Disconnect, sizeof(IO_DISCONNECT_INTERRUPT_PARAMETERS));

		Disconnect.Version = CONNECT_MESSAGE_BASED;
		Disconnect.ConnectionContext.InterruptObject = (PKINTERRUPT)pdx->InterruptObject;
		IoDisconnectInterruptEx(&Disconnect);	
	}


	if (pdx->bar0) {
		MmUnmapIoSpace(pdx ->bar0 , pdx->bar_size);
		DbgPrint("MmUnmapIoSpace");
	}


	DbgPrint("Remove Device");
	IoDeleteSymbolicLink(&pdx->ustrSymLinkName);

	if (pdx->NextStackDevice)
	{
		IoDetachDevice(pdx->NextStackDevice);
	}

	IoDeleteDevice(pdx->fdo);

	return status;
}





NTSTATUS WinNVMePnp(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	NTSTATUS			status = STATUS_SUCCESS;
	PDEVICE_EXTENSION	pdx = (PDEVICE_EXTENSION)fdo->DeviceExtension;
	PIO_STACK_LOCATION	stack = IoGetCurrentIrpStackLocation(Irp);
	static NTSTATUS(*fcntab[])(PDEVICE_EXTENSION pdx, PIRP Irp) =
	{
		HandleStartDevice,  // IRP_MN_START_DEVICE
		DefaultPnpHandler,  // IRP_MN_QUERY_REMOVE_DEVICE
		HandleRemoveDevice, // IRP_MN_REMOVE_DEVICE
		DefaultPnpHandler,  // IRP_MN_CANCEL_REMOVE_DEVICE
		DefaultPnpHandler,  // IRP_MN_STOP_DEVICE
		DefaultPnpHandler,  // IRP_MN_QUERY_STOP_DEVICE
		DefaultPnpHandler,  // IRP_MN_CANCEL_STOP_DEVICE
		DefaultPnpHandler,  // IRP_MN_QUERY_DEVICE_RELATIONS
		DefaultPnpHandler,  // IRP_MN_QUERY_INTERFACE
		DefaultPnpHandler,  // IRP_MN_QUERY_CAPABILITIES
		DefaultPnpHandler,  // IRP_MN_QUERY_RESOURCES
		DefaultPnpHandler,  // IRP_MN_QUERY_RESOURCE_REQUIREMENTS
		DefaultPnpHandler,  // IRP_MN_QUERY_DEVICE_TEXT
		DefaultPnpHandler,  // IRP_MN_FILTER_RESOURCE_REQUIREMENTS
		DefaultPnpHandler,  //
		DefaultPnpHandler,  // IRP_MN_READ_CONFIG
		DefaultPnpHandler,  // IRP_MN_WRITE_CONFIG
		DefaultPnpHandler,  // IRP_MN_EJECT
		DefaultPnpHandler,  // IRP_MN_SET_LOCK
		DefaultPnpHandler,  // IRP_MN_QUERY_ID
		DefaultPnpHandler,  // IRP_MN_QUERY_PNP_DEVICE_STATE
		DefaultPnpHandler,  // IRP_MN_QUERY_BUS_INFORMATION
		DefaultPnpHandler,  // IRP_MN_DEVICE_USAGE_NOTIFICATION
		DefaultPnpHandler,  // IRP_MN_SURPRISE_REMOVAL
	};
	static char* fcnname[] =
	{
		"IRP_MN_START_DEVICE",
		"IRP_MN_QUERY_REMOVE_DEVICE",
		"IRP_MN_REMOVE_DEVICE",
		"IRP_MN_CANCEL_REMOVE_DEVICE",
		"IRP_MN_STOP_DEVICE",
		"IRP_MN_QUERY_STOP_DEVICE",
		"IRP_MN_CANCEL_STOP_DEVICE",
		"IRP_MN_QUERY_DEVICE_RELATIONS",
		"IRP_MN_QUERY_INTERFACE",
		"IRP_MN_QUERY_CAPABILITIES",
		"IRP_MN_QUERY_RESOURCES",
		"IRP_MN_QUERY_RESOURCE_REQUIREMENTS",
		"IRP_MN_QUERY_DEVICE_TEXT",
		"IRP_MN_FILTER_RESOURCE_REQUIREMENTS",
		"",
		"IRP_MN_READ_CONFIG",
		"IRP_MN_WRITE_CONFIG",
		"IRP_MN_EJECT",
		"IRP_MN_SET_LOCK",
		"IRP_MN_QUERY_ID",
		"IRP_MN_QUERY_PNP_DEVICE_STATE",
		"IRP_MN_QUERY_BUS_INFORMATION",
		"IRP_MN_DEVICE_USAGE_NOTIFICATION",
		"IRP_MN_SURPRISE_REMOVAL",
	};
	ULONG				fcn = stack->MinorFunction;

	if (fcn >= arraysize(fcntab))
	{   
		status = DefaultPnpHandler(pdx, Irp);
		return status;
	}

	status = (*fcntab[fcn])(pdx, Irp);

	return status;
}





NTSTATUS WinNVMeDispatchRoutine(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	UNREFERENCED_PARAMETER(fdo);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0; 
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}





NTSTATUS ReadWriteConfigSpace(
	IN PDEVICE_OBJECT DeviceObject,
	IN ULONG ReadOrWrite, // 0 for read 1 for write
	IN PVOID Buffer,
	IN ULONG Offset,
	IN ULONG Length
)
{
	KEVENT				event;
	NTSTATUS			status;
	PIRP				irp;
	IO_STATUS_BLOCK		ioStatusBlock;
	PIO_STACK_LOCATION	irpStack;
	PDEVICE_OBJECT		targetObject;

	PAGED_CODE();

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	targetObject = IoGetAttachedDeviceReference(DeviceObject);

	irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, targetObject, NULL, 0, NULL, &event, &ioStatusBlock);

	if (irp == NULL)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto End;
	}

	irpStack = IoGetNextIrpStackLocation(irp);

	if (ReadOrWrite == 0)
	{
		irpStack->MinorFunction = IRP_MN_READ_CONFIG;
	}
	else
	{
		irpStack->MinorFunction = IRP_MN_WRITE_CONFIG;
	}

	irpStack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
	irpStack->Parameters.ReadWriteConfig.Buffer = Buffer;
	irpStack->Parameters.ReadWriteConfig.Offset = Offset;
	irpStack->Parameters.ReadWriteConfig.Length = Length;

	//
	// Initialize the status to error in case the bus driver does not
	// set it correctly.
	//

	irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

	status = IoCallDriver(targetObject, irp);

	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = ioStatusBlock.Status;
	}

End:
	// Done with reference
	ObDereferenceObject(targetObject);

	return status;
}





NTSTATUS WinNVMeDeviceControl(IN PDEVICE_OBJECT fdo, IN PIRP Irp)
{
	NTSTATUS			status = STATUS_SUCCESS;
	UNREFERENCED_PARAMETER(fdo);
	UNREFERENCED_PARAMETER(Irp);


	return status;
}





void WinNVMeUnload(IN PDRIVER_OBJECT DriverObject)
{
	UNREFERENCED_PARAMETER(DriverObject);



}





void WinNVMeDelay(long long millsecond)
{
	LARGE_INTEGER	delayValue, delayTrue;
	NTSTATUS		ntRet;

	// 10*1000*1000 is 1 second, so 10*1000 is 1 millsecond
	delayValue.QuadPart = 10 * 1000 * millsecond; // 320 millisecond
	delayTrue.QuadPart = -(delayValue.QuadPart);
	ntRet = KeDelayExecutionThread(KernelMode, FALSE, &delayTrue);
}

#if 0
NTSTATUS ReadWriteConfigSpace(
	IN PDEVICE_OBJECT DeviceObject,
	IN ULONG ReadOrWrite, // 0 for read 1 for write
	IN PVOID Buffer,
	IN ULONG Offset,
	IN ULONG Length
)
{
	KEVENT				event;
	NTSTATUS			status;
	PIRP				irp;
	IO_STATUS_BLOCK		ioStatusBlock;
	PIO_STACK_LOCATION	irpStack;
	PDEVICE_OBJECT		targetObject;

	PAGED_CODE();

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	targetObject = IoGetAttachedDeviceReference(DeviceObject);

	irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, targetObject, NULL, 0, NULL, &event, &ioStatusBlock);

	if (irp == NULL)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto End;
	}

	irpStack = IoGetNextIrpStackLocation(irp);

	if (ReadOrWrite == 0)
	{
		irpStack->MinorFunction = IRP_MN_READ_CONFIG;
	}
	else
	{
		irpStack->MinorFunction = IRP_MN_WRITE_CONFIG;
	}

	irpStack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
	irpStack->Parameters.ReadWriteConfig.Buffer = Buffer;
	irpStack->Parameters.ReadWriteConfig.Offset = Offset;
	irpStack->Parameters.ReadWriteConfig.Length = Length;

	//
	// Initialize the status to error in case the bus driver does not
	// set it correctly.
	//

	irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

	status = IoCallDriver(targetObject, irp);

	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = ioStatusBlock.Status;
	}

End:
	// Done with reference
	ObDereferenceObject(targetObject);

	return status;
}
#endif