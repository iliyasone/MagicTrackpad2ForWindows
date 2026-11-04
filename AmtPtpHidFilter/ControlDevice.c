// ControlDevice.c: control device routines

#include <Driver.h>
#include "ControlDevice.tmh"

// Minimum time between two actuations, in 100 ns units (20 ms, as LinuxMagicForce uses).
#define ACTUATE_MIN_GAP (20 * 10000ULL)

static NTSTATUS
PtpFilterControlActuate(
    _In_ PDRIVER_CONTEXT DriverContext,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength
)
{
	NTSTATUS status;
	PULONG pInputBuffer = NULL;
	ULONG feedback;
	ULONGLONG elapsed;
	LARGE_INTEGER delay;

	// The input is optional: without it the actuator fires with the current FeedbackClick setting.
	feedback = DriverContext->FeedbackClick;
	if (InputBufferLength != 0) {
		status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID*)&pInputBuffer, NULL);
		if (!NT_SUCCESS(status)) {
			TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestRetrieveInputBuffer failed: %!STATUS!", status);
			return status;
		}
		feedback = *pInputBuffer & 0xFFFFFF;
	}

	// The lock keeps the trackpad from being torn down during the send and serializes
	// actuations from both control devices.
	WdfWaitLockAcquire(DriverContext->HapticLock, NULL);

	if (DriverContext->HapticTrackpad == NULL) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! No trackpad started");
		status = STATUS_DEVICE_NOT_CONNECTED;
		goto Exit;
	}

	elapsed = KeQueryInterruptTime() - DriverContext->LastActuateTime;
	if (elapsed < ACTUATE_MIN_GAP) {
		delay.QuadPart = -(LONGLONG)(ACTUATE_MIN_GAP - elapsed);
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}
	DriverContext->LastActuateTime = KeQueryInterruptTime();

	status = PtpFilterActuate(DriverContext->HapticTrackpad, feedback);

Exit:
	WdfWaitLockRelease(DriverContext->HapticLock);
	return status;
}

VOID
PtpFilterEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;
	WDFDEVICE controlDevice;
	WDFDRIVER driver;
	PDRIVER_CONTEXT driverContext;
	PULONG pOutputBuffer = NULL;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);
	
	controlDevice = WdfIoQueueGetDevice(Queue);
	driver = WdfDeviceGetDriver(controlDevice);
	driverContext = PtpFilterDriverGetContext(driver);

    switch (IoControlCode)
    {
    case IOCTL_PTPFILTER_RELOAD_SETTINGS:
		if (driverContext->CDFirstDevice == NULL) {
			TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! CDFirstDevice == NULL");
			status = STATUS_INVALID_DEVICE_STATE;
		} else {
			PtpFilterReadSettings(driverContext);
			PtpFilterSetHapticFeedback(driverContext->CDFirstDevice, driverContext->FeedbackClick, driverContext->FeedbackRelease);
		}
        break;
		
    case IOCTL_PTPFILTER_GET_BATTERY:
        WdfWaitLockAcquire(driverContext->HapticLock, NULL);
        status = STATUS_DEVICE_NOT_CONNECTED;
        if (driverContext->HapticTrackpad != NULL) {
            BATTERY_CONTEXT* battery = &PtpFilterGetContext(driverContext->HapticTrackpad)->Battery;
            LONG64 sample = InterlockedCompareExchange64(&battery->Cached, 0, 0);
            LONG epoch = battery->Epoch;
            status = STATUS_DEVICE_NOT_READY;
            if ((epoch & 1) && (ULONG)(sample >> 8) == (ULONG)epoch) {
                status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), (PVOID*)&pOutputBuffer, NULL);
                if (NT_SUCCESS(status)) {
                    *pOutputBuffer = (ULONG)(sample & 0xff);
                    bytesReturned = sizeof(ULONG);
                }
            }
        }
        WdfWaitLockRelease(driverContext->HapticLock);
        break;

	case IOCTL_PTPFILTER_ACTUATE:
		status = PtpFilterControlActuate(driverContext, Request, InputBufferLength);
		break;

    default:
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, 
            "Unsupported IOCTL: 0x%08lX", IoControlCode);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

//
// Hold (IOCTL_PTPFILTER_HOLD, see ControlDevice.h).
//

static EVT_WDF_REQUEST_CANCEL PtpFilterEvtHoldCanceled;

static VOID
PtpFilterEvtHoldCanceled(
    _In_ WDFREQUEST Request
)
{
	PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfGetDriver());

	// The app let go of the hold: Windows sees the pad as it is again.
	WdfSpinLockAcquire(driverContext->HoldLock);
	if (driverContext->HoldRequest == Request) {
		driverContext->HoldRequest = NULL;
		driverContext->HiddenContact = HOLD_NO_CONTACT;
	}
	WdfSpinLockRelease(driverContext->HoldLock);

	WdfRequestComplete(Request, STATUS_CANCELLED);
}

static NTSTATUS
PtpFilterControlHold(
    _In_ PDRIVER_CONTEXT DriverContext,
    _In_ WDFREQUEST Request
)
{
	NTSTATUS status;
	PULONG pContact = NULL;

	status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID*)&pContact, NULL);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! WdfRequestRetrieveInputBuffer failed: %!STATUS!", status);
		return status;
	}

	WdfSpinLockAcquire(DriverContext->HoldLock);
	if (DriverContext->HoldRequest != NULL) {
		status = STATUS_DEVICE_BUSY;
	} else if (!DriverContext->RawButton) {
		status = STATUS_DEVICE_NOT_READY;
	} else {
		// The Ex variant never calls the cancel routine from here, so holding the lock is safe.
		status = WdfRequestMarkCancelableEx(Request, PtpFilterEvtHoldCanceled);
		if (NT_SUCCESS(status)) {
			DriverContext->HoldRequest = Request;
			DriverContext->HiddenContact = *pContact;
			DriverContext->HiddenLiftSent = FALSE;
			status = STATUS_PENDING;
		}
	}
	WdfSpinLockRelease(DriverContext->HoldLock);
	return status;
}

// Ends a pending hold with Status: the button came up, or the trackpad went away.
static VOID
PtpFilterHoldComplete(
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status
)
{
	// STATUS_CANCELLED: the cancel routine runs and completes it.
	if (WdfRequestUnmarkCancelable(Request) != STATUS_CANCELLED) {
		WdfRequestComplete(Request, Status);
	}
}

VOID
PtpFilterHoldApply(
    _In_ PDRIVER_CONTEXT DriverContext,
    _In_ BOOLEAN Button,
    _Inout_ PPTP_REPORT Report
)
{
	WDFREQUEST released = NULL;
	BOOLEAN touching = FALSE;
	UCHAR i, kept = 0;

	WdfSpinLockAcquire(DriverContext->HoldLock);
	DriverContext->RawButton = Button;

	if (DriverContext->HoldRequest != NULL) {
		if (Button) {
			Report->IsButtonClicked = 0;
		} else {
			released = DriverContext->HoldRequest;
			DriverContext->HoldRequest = NULL;
		}
	}

	// The held finger: reported lifted once, then left out until it lifts for real, so that
	// it neither points nor counts in a gesture, even after the button comes up.
	if (DriverContext->HiddenContact != HOLD_NO_CONTACT) {
		for (i = 0; i < Report->ContactCount && i < PTP_MAX_CONTACT_POINTS; i++) {
			PTP_CONTACT contact = Report->Contacts[i];
			if (contact.ContactID == DriverContext->HiddenContact) {
				if (contact.TipSwitch) {
					touching = TRUE;
				}
				Report->MechanicalForce -= contact.Pressure;
				if (DriverContext->HiddenLiftSent) {
					continue;
				}
				DriverContext->HiddenLiftSent = TRUE;
				contact.TipSwitch = 0;
				contact.Width = 0;
				contact.Height = 0;
				contact.Pressure = 0;
			}
			Report->Contacts[kept++] = contact;
		}
		for (i = kept; i < Report->ContactCount && i < PTP_MAX_CONTACT_POINTS; i++) {
			RtlZeroMemory(&Report->Contacts[i], sizeof(PTP_CONTACT));
		}
		Report->ContactCount = kept;
		if (!touching) {
			DriverContext->HiddenContact = HOLD_NO_CONTACT;
		}
	}
	WdfSpinLockRelease(DriverContext->HoldLock);

	if (released != NULL) {
		PtpFilterHoldComplete(released, STATUS_SUCCESS);
	}
}

// The trackpad went away: a pending hold ends, and nothing stays hidden.
static VOID
PtpFilterHoldEnd(
    _In_ PDRIVER_CONTEXT DriverContext
)
{
	WDFREQUEST request;

	WdfSpinLockAcquire(DriverContext->HoldLock);
	request = DriverContext->HoldRequest;
	DriverContext->HoldRequest = NULL;
	DriverContext->HiddenContact = HOLD_NO_CONTACT;
	DriverContext->RawButton = FALSE;
	WdfSpinLockRelease(DriverContext->HoldLock);

	if (request != NULL) {
		PtpFilterHoldComplete(request, STATUS_DEVICE_NOT_CONNECTED);
	}
}

// Haptic device: opened by unprivileged apps, so it accepts IOCTL_PTPFILTER_ACTUATE and
// IOCTL_PTPFILTER_HOLD and nothing else. Its queue is parallel: a hold stays pending while
// actuations go through.
VOID
PtpFilterEvtIoHapticDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
	NTSTATUS status;
	PDRIVER_CONTEXT driverContext;

	UNREFERENCED_PARAMETER(OutputBufferLength);

	driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(WdfIoQueueGetDevice(Queue)));

	if (IoControlCode == IOCTL_PTPFILTER_ACTUATE) {
		status = PtpFilterControlActuate(driverContext, Request, InputBufferLength);
	} else if (IoControlCode == IOCTL_PTPFILTER_HOLD) {
		status = PtpFilterControlHold(driverContext, Request);
		if (status == STATUS_PENDING) {
			return;
		}
	} else {
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "Unsupported IOCTL on the haptic device: 0x%08lX", IoControlCode);
		status = STATUS_INVALID_DEVICE_REQUEST;
	}

	WdfRequestComplete(Request, status);
}

NTSTATUS
PtpFilterControlDeviceQueueInitialize(
    _In_ WDFDEVICE Device,
    _In_ PFN_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl,
    _In_ WDF_IO_QUEUE_DISPATCH_TYPE DispatchType
)
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    NTSTATUS status;
    WDFQUEUE queue;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, DispatchType);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    status = WdfIoQueueCreate(Device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &queue);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, 
            "WdfIoQueueCreate failed: %!STATUS!", status);
    }

    return status;
}

static NTSTATUS
PtpFilterCreateNamedControlDevice(
    _In_ WDFDRIVER Driver,
    _In_ PCUNICODE_STRING DeviceName,
    _In_ PCUNICODE_STRING SymLink,
    _In_ PCUNICODE_STRING Sddl,
    _In_ PFN_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl,
    _In_ WDF_IO_QUEUE_DISPATCH_TYPE DispatchType,
    _In_opt_ PWDF_FILEOBJECT_CONFIG FileObjectConfig,
    _Out_ WDFDEVICE* Device
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT deviceInit = NULL;
    WDFDEVICE controlDevice;
    WDF_OBJECT_ATTRIBUTES attributes;

    *Device = NULL;

    deviceInit = WdfControlDeviceInitAllocate(Driver, Sddl);
    if (deviceInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, 
            "WdfControlDeviceInitAllocate failed");
        return status;
    }

    status = WdfDeviceInitAssignName(deviceInit, DeviceName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, 
            "WdfDeviceInitAssignName failed: %!STATUS!", status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    if (FileObjectConfig != NULL) {
        WdfDeviceInitSetFileObjectConfig(deviceInit, FileObjectConfig, WDF_NO_OBJECT_ATTRIBUTES);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    status = WdfDeviceCreate(&deviceInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        // E.g. STATUS_OBJECT_NAME_COLLISION. On failure the init is still ours to free.
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, 
            "WdfDeviceCreate (control) failed: %!STATUS!", status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(controlDevice, SymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, 
            "WdfDeviceCreateSymbolicLink failed: %!STATUS!", status);
        WdfObjectDelete(controlDevice); 
        return status;
    }

    status = PtpFilterControlDeviceQueueInitialize(controlDevice, EvtIoDeviceControl, DispatchType);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WdfControlFinishInitializing(controlDevice);

    *Device = controlDevice;
    return STATUS_SUCCESS;
}

NTSTATUS
PtpFilterCreateControlDevice(
    _In_ WDFDRIVER Driver
)
{
    NTSTATUS status;
    WDFDEVICE controlDevice;
    PDRIVER_CONTEXT driverContext;
    
    DECLARE_CONST_UNICODE_STRING(controlDeviceName, L"\\Device\\AmtPtpControlDeviceUm");
    DECLARE_CONST_UNICODE_STRING(controlDeviceSymLink, L"\\DosDevices\\AmtPtpControlDeviceUm");

    status = PtpFilterCreateNamedControlDevice(Driver, &controlDeviceName, &controlDeviceSymLink,
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, PtpFilterEvtIoDeviceControl, WdfIoQueueDispatchSequential, NULL, &controlDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    driverContext = PtpFilterDriverGetContext(Driver);
    driverContext->ControlDevice = controlDevice;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, 
        "Control device created successfully");
    
    return STATUS_SUCCESS;
}

VOID
PtpFilterDeleteControlDevice(
    _In_ WDFOBJECT Context
)
{
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext((WDFDRIVER)Context);

    if (driverContext->ControlDevice != NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Deleting Control Device");
        WdfObjectDelete(driverContext->ControlDevice);
        driverContext->ControlDevice = NULL;
    }
}

//
// Haptic device lifetime. It exists while a trackpad exists or an app holds it open, so a
// handle held across a reconnect stays valid and the name is never taken by a deleted device.
// All of it runs at PASSIVE_LEVEL under HapticLock.
//

static VOID
PtpFilterDeleteHapticDeviceIfUnused(
    _In_ PDRIVER_CONTEXT DriverContext
)
{
    if (DriverContext->HapticDevice != NULL && DriverContext->Trackpads == 0 && DriverContext->HapticHandles == 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Deleting Haptic Device");
        WdfObjectDelete(DriverContext->HapticDevice);
        DriverContext->HapticDevice = NULL;
    }
}

static EVT_WDF_DEVICE_FILE_CREATE PtpFilterEvtHapticFileCreate;
static EVT_WDF_FILE_CLOSE PtpFilterEvtHapticFileClose;

static VOID
PtpFilterEvtHapticFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(Device));

    UNREFERENCED_PARAMETER(FileObject);

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (Device == driverContext->HapticDevice) {
        driverContext->HapticHandles++;
    } else {
        // The open raced with the deletion of this device.
        status = STATUS_DELETE_PENDING;
    }
    WdfWaitLockRelease(driverContext->HapticLock);

    WdfRequestComplete(Request, status);
}

static IO_WORKITEM_ROUTINE PtpFilterHapticDeleteWorkItem;

static VOID
PtpFilterHapticDeleteWorkItem(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context
)
{
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfGetDriver());

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (driverContext->HapticDevice != NULL && WdfDeviceWdmGetDeviceObject(driverContext->HapticDevice) == DeviceObject) {
        PtpFilterDeleteHapticDeviceIfUnused(driverContext);
    }
    WdfWaitLockRelease(driverContext->HapticLock);

    if (Context != NULL) {
        IoFreeWorkItem((PIO_WORKITEM)Context);
    }
}

static VOID
PtpFilterEvtHapticFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(device));
    PIO_WORKITEM workItem;

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (device == driverContext->HapticDevice) {
        driverContext->HapticHandles--;
        // The last handle closed after the last trackpad went away. Delete the device from a
        // work item rather than from its own close callback; the work item references the
        // device object, so the driver can't unload before it runs.
        if (driverContext->HapticHandles == 0 && driverContext->Trackpads == 0) {
            workItem = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(device));
            if (workItem != NULL) {
                IoQueueWorkItem(workItem, PtpFilterHapticDeleteWorkItem, DelayedWorkQueue, workItem);
            } else {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "IoAllocateWorkItem failed, Haptic Device kept until the next trackpad");
            }
        }
    }
    WdfWaitLockRelease(driverContext->HapticLock);
}

VOID
PtpFilterHapticTrackpadAdded(
    _In_ WDFDRIVER Driver
)
{
    NTSTATUS status;
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(Driver);
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFDEVICE hapticDevice;
    PCUNICODE_STRING hapticDeviceSddl;

    DECLARE_CONST_UNICODE_STRING(hapticDeviceName, L"\\Device\\AmtPtpHapticDeviceUm");
    DECLARE_CONST_UNICODE_STRING(hapticDeviceSymLink, L"\\DosDevices\\AmtPtpHapticDeviceUm");
    // SYSTEM and Administrators: all access. Interactive users: read/write, so that an app
    // running in the user's session can fire the actuator without elevation.
    DECLARE_CONST_UNICODE_STRING(hapticDeviceSddlNonAdmin, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");

    // Non-admins may open \\.\AmtPtpHapticDeviceUm only when the AllowNonAdminAccess
    // setting is present and non-zero; otherwise keep it admin-only, like the control device.
    hapticDeviceSddl = (PtpFilterReadSettingValue(L"AllowNonAdminAccess", 0) != 0)
        ? &hapticDeviceSddlNonAdmin
        : &SDDL_DEVOBJ_SYS_ALL_ADM_ALL;

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);

    driverContext->Trackpads++;

    // The haptic device is optional: the trackpad, settings and battery work without it.
    // If it can't be created now, the next trackpad device retries.
    if (driverContext->HapticDevice == NULL) {
        WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, PtpFilterEvtHapticFileCreate, PtpFilterEvtHapticFileClose, WDF_NO_EVENT_CALLBACK);
        status = PtpFilterCreateNamedControlDevice(Driver, &hapticDeviceName, &hapticDeviceSymLink,
            hapticDeviceSddl, PtpFilterEvtIoHapticDeviceControl, WdfIoQueueDispatchParallel, &fileConfig, &hapticDevice);
        if (NT_SUCCESS(status)) {
            driverContext->HapticDevice = hapticDevice;
            driverContext->HapticHandles = 0;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Haptic device created successfully");
        } else {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, 
                "Haptic device not created: %!STATUS!, continuing without it", status);
        }
    }

    WdfWaitLockRelease(driverContext->HapticLock);
}

VOID
PtpFilterHapticTrackpadStarted(
    _In_ WDFDEVICE Device
)
{
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(Device));

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (driverContext->HapticTrackpad == NULL) {
        driverContext->HapticTrackpad = Device;
    }
    WdfWaitLockRelease(driverContext->HapticLock);
}

VOID
PtpFilterHapticTrackpadStopped(
    _In_ WDFDEVICE Device
)
{
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(Device));

    // Waits for an actuation in flight on this trackpad to finish.
    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (driverContext->HapticTrackpad == Device) {
        driverContext->HapticTrackpad = NULL;
    }
    WdfWaitLockRelease(driverContext->HapticLock);

    PtpFilterHoldEnd(driverContext);
}

VOID
PtpFilterHapticTrackpadRemoved(
    _In_ WDFDEVICE Device
)
{
    PDRIVER_CONTEXT driverContext = PtpFilterDriverGetContext(WdfDeviceGetDriver(Device));

    WdfWaitLockAcquire(driverContext->HapticLock, NULL);
    if (driverContext->HapticTrackpad == Device) {
        driverContext->HapticTrackpad = NULL;
    }
    driverContext->Trackpads--;
    // With a handle still open the device stays, and the last close deletes it.
    PtpFilterDeleteHapticDeviceIfUnused(driverContext);
    WdfWaitLockRelease(driverContext->HapticLock);
}
