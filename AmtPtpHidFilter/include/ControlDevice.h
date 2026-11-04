// ControlDevice.h: control device related definitions

#pragma once

EXTERN_C_START

NTSTATUS
PtpFilterCreateControlDevice(
    _In_ WDFDRIVER Driver
);

VOID
PtpFilterDeleteControlDevice(
    _In_ WDFOBJECT Context
);

// \\.\AmtPtpHapticDeviceUm lifetime, driven by the trackpad devices:
// Added after WdfDeviceCreate, Started once the HID transport is usable,
// Stopped in self-managed I/O cleanup, Removed in the device's cleanup callback.
VOID
PtpFilterHapticTrackpadAdded(
    _In_ WDFDRIVER Driver
);

VOID
PtpFilterHapticTrackpadStarted(
    _In_ WDFDEVICE Device
);

VOID
PtpFilterHapticTrackpadStopped(
    _In_ WDFDEVICE Device
);

VOID
PtpFilterHapticTrackpadRemoved(
    _In_ WDFDEVICE Device
);

//
// IOCTLs
//

#define PTPFILTER_CTL_CODE(x) CTL_CODE(FILE_DEVICE_UNKNOWN, x, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PTPFILTER_RELOAD_SETTINGS  PTPFILTER_CTL_CODE(0x800)
#define IOCTL_PTPFILTER_GET_BATTERY      PTPFILTER_CTL_CODE(0x801)

// IOCTL_PTPFILTER_ACTUATE: fires the haptic actuator once (Bluetooth MT2 only).
// Input (optional): ULONG in the encoding of the FeedbackClick setting, e.g. 0x040415 (light),
// 0x060617 (medium), 0x08081E (firm). Without input, the current FeedbackClick setting is used.
// Bits 8-15 are capped at 0x20 and bits 16-23 at 0x40 (report bytes 6 and 11; LinuxMagicForce
// calls higher values hardware-stress settings), and actuations are at least 20 ms apart.
// Fails with STATUS_DEVICE_NOT_CONNECTED while no trackpad is started; a handle to
// \\.\AmtPtpHapticDeviceUm stays valid across a reconnect.
// Accepted by \\.\AmtPtpControlDeviceUm (administrators) and by \\.\AmtPtpHapticDeviceUm,
// which accepts no other IOCTL. \\.\AmtPtpHapticDeviceUm is openable by interactive users only
// when the AllowNonAdminAccess registry setting is present and non-zero; otherwise it stays
// admin-only. FILE_WRITE_ACCESS makes the I/O manager require a handle opened for writing, so
// the device DACL is what grants the call.
#define IOCTL_PTPFILTER_ACTUATE          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// IOCTL_PTPFILTER_HOLD: for an app that gives a press in part of the pad a meaning of its own
// (a push-to-talk key, say) while the rest of the pad keeps working. Input: ULONG, the
// ContactID of the finger pressing, as in the PTP report. Accepted only while the button is
// down; from the next report until the button comes up, Windows sees the button up, and that
// contact lifting and then gone; after that, the contact shows again as a new touch. The request stays pending and completes
// with STATUS_SUCCESS when the button comes up; cancel it (or close the handle) to end the hold
// early. Fails at once with STATUS_DEVICE_NOT_READY when the button is already up, and with
// STATUS_DEVICE_BUSY while another hold is pending. Only on \\.\AmtPtpHapticDeviceUm.
#define IOCTL_PTPFILTER_HOLD             CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define HOLD_NO_CONTACT                  ((ULONG)-1)

// From the input path, at DISPATCH_LEVEL: applies a hold to a report before it goes up.
VOID
PtpFilterHoldApply(
    _In_ PDRIVER_CONTEXT DriverContext,
    _In_ BOOLEAN Button,
    _Inout_ PPTP_REPORT Report
);

EXTERN_C_END
