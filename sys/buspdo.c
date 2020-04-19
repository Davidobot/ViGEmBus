/*
* Virtual Gamepad Emulation Framework - Windows kernel-mode bus driver
*
* MIT License
*
* Copyright (c) 2016-2019 Nefarius Software Solutions e.U. and Contributors
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/


#include "busenum.h"
#include <wdmsec.h>
#include <usbioctl.h>
#include "buspdo.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Bus_CreatePdo)
#pragma alloc_text(PAGE, Bus_EvtDeviceListCreatePdo)
#pragma alloc_text(PAGE, Pdo_EvtDevicePrepareHardware)
#endif

NTSTATUS Bus_EvtDeviceListCreatePdo(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    PWDFDEVICE_INIT ChildInit)
{
    PPDO_IDENTIFICATION_DESCRIPTION pDesc;

    PAGED_CODE();

    pDesc = CONTAINING_RECORD(IdentificationDescription, PDO_IDENTIFICATION_DESCRIPTION, Header);

    return Bus_CreatePdo(WdfChildListGetDevice(DeviceList), ChildInit, pDesc);
}

//
// Compares two children on the bus based on their serial numbers.
// 
BOOLEAN Bus_EvtChildListIdentificationDescriptionCompare(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER FirstIdentificationDescription,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER SecondIdentificationDescription)
{
    PPDO_IDENTIFICATION_DESCRIPTION lhs, rhs;

    UNREFERENCED_PARAMETER(DeviceList);

    lhs = CONTAINING_RECORD(FirstIdentificationDescription,
        PDO_IDENTIFICATION_DESCRIPTION,
        Header);
    rhs = CONTAINING_RECORD(SecondIdentificationDescription,
        PDO_IDENTIFICATION_DESCRIPTION,
        Header);

    return (lhs->SerialNo == rhs->SerialNo) ? TRUE : FALSE;
}

//
// Creates and initializes a PDO (child).
// 
NTSTATUS Bus_CreatePdo(
    _In_ WDFDEVICE Device,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PPDO_IDENTIFICATION_DESCRIPTION Description)
{
    NTSTATUS                        status;
    PPDO_DEVICE_DATA                pdoData;
    WDFDEVICE                       hChild = NULL;
    WDF_DEVICE_PNP_CAPABILITIES     pnpCaps;
    WDF_DEVICE_POWER_CAPABILITIES   powerCaps;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES           pdoAttributes;
    WDF_IO_QUEUE_CONFIG             defaultPdoQueueConfig;
    WDFQUEUE                        defaultPdoQueue;
    UNICODE_STRING                  deviceDescription;
    VIGEM_BUS_INTERFACE             busInterface;
    WDF_OBJECT_ATTRIBUTES           attributes;
    WDF_IO_QUEUE_CONFIG             usbInQueueConfig;
    WDF_IO_QUEUE_CONFIG             notificationsQueueConfig;

    DECLARE_CONST_UNICODE_STRING(deviceLocation, L"Virtual Gamepad Emulation Bus");
    DECLARE_UNICODE_STRING_SIZE(buffer, MAX_INSTANCE_ID_LEN);
    // reserve space for device id
    DECLARE_UNICODE_STRING_SIZE(deviceId, MAX_INSTANCE_ID_LEN);


    PAGED_CODE();


    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    //
    // Get the FDO interface ASAP to report progress to bus
    // 
    status = WdfFdoQueryForInterface(Device,
        &GUID_VIGEM_INTERFACE_PDO,
        (PINTERFACE)&busInterface,
        sizeof(VIGEM_BUS_INTERFACE),
        VIGEM_BUS_INTERFACE_VERSION,
        NULL);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfFdoQueryForInterface failed with status %!STATUS!",
            status);
        return status;
    }

    // set device type
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    // Bus is power policy owner
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);

#pragma region Enter RAW device mode

    status = WdfPdoInitAssignRawDevice(DeviceInit, &GUID_DEVCLASS_VIGEM_RAWPDO);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfPdoInitAssignRawDevice failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, TRUE);

    status = WdfDeviceInitAssignSDDLString(DeviceInit, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfDeviceInitAssignSDDLString failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

#pragma endregion

#pragma region Prepare PDO

    // set parameters matching desired target device
    switch (Description->TargetType)
    {
        //
        // A Xbox 360 device was requested
        // 
    case Xbox360Wired:

        status = Xusb_PreparePdo(
            DeviceInit,
            Description->VendorId,
            Description->ProductId,
            &deviceId,
            &deviceDescription);

        if (!NT_SUCCESS(status))
            goto endCreatePdo;

        break;

        //
        // A Sony DualShock 4 device was requested
        // 
    case DualShock4Wired:

        status = Ds4_PreparePdo(DeviceInit, &deviceId, &deviceDescription);

        if (!NT_SUCCESS(status))
            goto endCreatePdo;

        break;

        //
        // A Xbox One device was requested
        // 
    case XboxOneWired:

        status = Xgip_PreparePdo(DeviceInit, &deviceId, &deviceDescription);

        if (!NT_SUCCESS(status))
            goto endCreatePdo;

        break;

    default:

        status = STATUS_INVALID_PARAMETER;

        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "Unknown target type: %d (%!STATUS!)",
            Description->TargetType,
            status);

        goto endCreatePdo;
    }

    // set device id
    status = WdfPdoInitAssignDeviceID(DeviceInit, &deviceId);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfPdoInitAssignDeviceID failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    // prepare instance id
    status = RtlUnicodeStringPrintf(&buffer, L"%02d", Description->SerialNo);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "RtlUnicodeStringPrintf failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    // set instance id
    status = WdfPdoInitAssignInstanceID(DeviceInit, &buffer);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfPdoInitAssignInstanceID failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    // set device description (for English operating systems)
    status = WdfPdoInitAddDeviceText(DeviceInit, &deviceDescription, &deviceLocation, 0x409);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfPdoInitAddDeviceText failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    // default locale is English
    // TODO: add more locales
    WdfPdoInitSetDefaultLocale(DeviceInit, 0x409);

#pragma endregion

#pragma region PNP/Power event callbacks

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    pnpPowerCallbacks.EvtDevicePrepareHardware = Pdo_EvtDevicePrepareHardware;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

#pragma endregion

    // NOTE: not utilized at the moment
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

#pragma region Create PDO

    // Add common device data context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, PDO_DEVICE_DATA);

    status = WdfDeviceCreate(&DeviceInit, &pdoAttributes, &hChild);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfDeviceCreate failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE,
        TRACE_BUSPDO,
        "Created PDO 0x%p",
        hChild);

    switch (Description->TargetType)
    {
        // Add XUSB-specific device data context
    case Xbox360Wired:
    {
        PXUSB_DEVICE_DATA xusbData = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, XUSB_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&xusbData);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_BUSPDO,
                "WdfObjectAllocateContext failed with status %!STATUS!",
                status);
            goto endCreatePdo;
        }

        break;
    }
    case DualShock4Wired:
    {
        PDS4_DEVICE_DATA ds4Data = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, DS4_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&ds4Data);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_BUSPDO,
                "WdfObjectAllocateContext failed with status %!STATUS!",
                status);
            goto endCreatePdo;
        }

        break;
    }
    case XboxOneWired:
    {
        PXGIP_DEVICE_DATA xgipData = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, XGIP_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&xgipData);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_BUSPDO,
                "WdfObjectAllocateContext failed with status %!STATUS!",
                status);
            goto endCreatePdo;
        }

        break;
    }
    default:
        break;
    }

#pragma endregion

#pragma region Expose USB Interface

    status = WdfDeviceCreateDeviceInterface(Device, (LPGUID)&GUID_DEVINTERFACE_USB_DEVICE, NULL);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfDeviceCreateDeviceInterface failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

#pragma endregion

#pragma region Set PDO contexts

    pdoData = PdoGetData(hChild);

    pdoData->BusInterface = busInterface;

    pdoData->SerialNo = Description->SerialNo;
    pdoData->TargetType = Description->TargetType;
    pdoData->OwnerProcessId = Description->OwnerProcessId;
    pdoData->VendorId = Description->VendorId;
    pdoData->ProductId = Description->ProductId;

    TraceEvents(TRACE_LEVEL_VERBOSE,
        TRACE_BUSPDO,
        "PDO Context properties: serial = %d, type = %d, pid = %d, vid = 0x%04X, pid = 0x%04X",
        pdoData->SerialNo,
        pdoData->TargetType,
        pdoData->OwnerProcessId,
        pdoData->VendorId,
        pdoData->ProductId);

    // Initialize additional contexts (if available)
    switch (Description->TargetType)
    {
    case Xbox360Wired:

        status = Xusb_AssignPdoContext(hChild);

        break;

    case DualShock4Wired:

        status = Ds4_AssignPdoContext(hChild, Description);

        break;

    case XboxOneWired:

        status = Xgip_AssignPdoContext(hChild);

        break;

    default:
        break;
    }

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "Couldn't initialize additional contexts: %!STATUS!",
            status);

        goto endCreatePdo;
    }

#pragma endregion

#pragma region Create Queues & Locks

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = hChild;

    // Create and assign queue for incoming interrupt transfer
    WDF_IO_QUEUE_CONFIG_INIT(&usbInQueueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(hChild, &usbInQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &pdoData->PendingUsbInRequests);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfIoQueueCreate (PendingUsbInRequests) failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

    // Create and assign queue for user-land notification requests
    WDF_IO_QUEUE_CONFIG_INIT(&notificationsQueueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(Device, &notificationsQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &pdoData->PendingNotificationRequests);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfIoQueueCreate (PendingNotificationRequests) failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

#pragma endregion 

#pragma region Default I/O queue setup

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&defaultPdoQueueConfig, WdfIoQueueDispatchParallel);

    defaultPdoQueueConfig.EvtIoInternalDeviceControl = Pdo_EvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(hChild, &defaultPdoQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultPdoQueue);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_BUSPDO,
            "WdfIoQueueCreate (Default) failed with status %!STATUS!",
            status);
        goto endCreatePdo;
    }

#pragma endregion

#pragma region PNP capabilities

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);

    pnpCaps.Removable = WdfTrue;
    pnpCaps.EjectSupported = WdfTrue;
    pnpCaps.SurpriseRemovalOK = WdfTrue;

    pnpCaps.Address = Description->SerialNo;
    pnpCaps.UINumber = Description->SerialNo;

    WdfDeviceSetPnpCapabilities(hChild, &pnpCaps);

#pragma endregion

#pragma region Power capabilities

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&powerCaps);

    powerCaps.DeviceD1 = WdfTrue;
    powerCaps.WakeFromD1 = WdfTrue;
    powerCaps.DeviceWake = PowerDeviceD1;

    powerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
    powerCaps.DeviceState[PowerSystemSleeping1] = PowerDeviceD1;
    powerCaps.DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemShutdown] = PowerDeviceD3;

    WdfDeviceSetPowerCapabilities(hChild, &powerCaps);

#pragma endregion

    endCreatePdo:
                TraceEvents(TRACE_LEVEL_INFORMATION,
                    TRACE_BUSPDO,
                    "BUS_PDO_REPORT_STAGE_RESULT Stage: ViGEmPdoCreate  [serial: %d, status: %!STATUS!]",
                    Description->SerialNo, status);

                BUS_PDO_REPORT_STAGE_RESULT(busInterface, ViGEmPdoCreate, Description->SerialNo, status);

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_BUSPDO, "%!FUNC! Exit with status %!STATUS!", status);

                return status;
}

//
// PDO power-up.
// 
NTSTATUS Pdo_EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PPDO_DEVICE_DATA    pdoData;
    NTSTATUS            status = STATUS_UNSUCCESSFUL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_BUSENUM, "%!FUNC! Entry");

    pdoData = PdoGetData(Device);

    switch (pdoData->TargetType)
    {
        // Expose XUSB interfaces
    case Xbox360Wired:

        status = Xusb_PrepareHardware(Device);

        break;

    case DualShock4Wired:

        status = Ds4_PrepareHardware(Device);

        break;

    case XboxOneWired:

        status = Xgip_PrepareHardware(Device);

        break;

    default:
        break;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION,
        TRACE_BUSPDO,
        "BUS_PDO_REPORT_STAGE_RESULT Stage: ViGEmPdoCreate  [serial: %d, status: %!STATUS!]",
        pdoData->SerialNo, status);

    BUS_PDO_REPORT_STAGE_RESULT(pdoData->BusInterface, ViGEmPdoPrepareHardware, pdoData->SerialNo, status);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_BUSPDO, "%!FUNC! Exit with status %!STATUS!", status);

    return status;
}

//
// Responds to IRP_MJ_INTERNAL_DEVICE_CONTROL requests sent to PDO.
// 
VOID Pdo_EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    // Regular buffers not used in USB communication
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS                status = STATUS_INVALID_PARAMETER;
    WDFDEVICE               hDevice;
    PIRP                    irp;
    PURB                    urb;
    PPDO_DEVICE_DATA        pdoData;
    PIO_STACK_LOCATION      irpStack;
    PXUSB_DEVICE_DATA       pXusbData;
    PUCHAR                  blobBuffer;


    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_BUSPDO, "%!FUNC! Entry");

    hDevice = WdfIoQueueGetDevice(Queue);
    pdoData = PdoGetData(hDevice);
    // No help from the framework available from here on
    irp = WdfRequestWdmGetIrp(Request);
    irpStack = IoGetCurrentIrpStackLocation(irp);

    switch (IoControlCode)
    {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            ">> IOCTL_INTERNAL_USB_SUBMIT_URB");

        urb = (PURB)URB_FROM_IRP(irp);

        switch (urb->UrbHeader.Function)
        {
        case URB_FUNCTION_CONTROL_TRANSFER:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_CONTROL_TRANSFER");

            switch (urb->UrbControlTransfer.SetupPacket[6])
            {
            case 0x04:
                if (pdoData->TargetType == Xbox360Wired)
                {
                    pXusbData = XusbGetData(hDevice);
                    blobBuffer = WdfMemoryGetBuffer(pXusbData->InterruptBlobStorage, NULL);
                    //
                    // Xenon magic
                    // 
                    RtlCopyMemory(
                        urb->UrbControlTransfer.TransferBuffer,
                        &blobBuffer[XUSB_BLOB_07_OFFSET],
                        0x04
                    );
                    status = STATUS_SUCCESS;
                }
                break;
            case 0x14:
                //
                // This is some weird USB 1.0 condition and _must fail_
                // 
                urb->UrbControlTransfer.Hdr.Status = USBD_STATUS_STALL_PID;
                status = STATUS_UNSUCCESSFUL;
                break;
            case 0x08:
                //
                // This is some weird USB 1.0 condition and _must fail_
                // 
                urb->UrbControlTransfer.Hdr.Status = USBD_STATUS_STALL_PID;
                status = STATUS_UNSUCCESSFUL;
                break;
            default:
                status = STATUS_SUCCESS;
                break;
            }

            break;

        case URB_FUNCTION_CONTROL_TRANSFER_EX:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_CONTROL_TRANSFER_EX");

            status = STATUS_UNSUCCESSFUL;

            break;

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER");

            status = UsbPdo_BulkOrInterruptTransfer(urb, hDevice, Request);

            break;

        case URB_FUNCTION_SELECT_CONFIGURATION:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_SELECT_CONFIGURATION");

            status = UsbPdo_SelectConfiguration(urb, pdoData);

            break;

        case URB_FUNCTION_SELECT_INTERFACE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_SELECT_INTERFACE");

            status = UsbPdo_SelectInterface(urb, pdoData);

            break;

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE");

            switch (urb->UrbControlDescriptorRequest.DescriptorType)
            {
            case USB_DEVICE_DESCRIPTOR_TYPE:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> USB_DEVICE_DESCRIPTOR_TYPE");

                status = UsbPdo_GetDeviceDescriptorType(urb, pdoData);

                break;

            case USB_CONFIGURATION_DESCRIPTOR_TYPE:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> USB_CONFIGURATION_DESCRIPTOR_TYPE");

                status = UsbPdo_GetConfigurationDescriptorType(urb, pdoData);

                break;

            case USB_STRING_DESCRIPTOR_TYPE:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> USB_STRING_DESCRIPTOR_TYPE");

                status = UsbPdo_GetStringDescriptorType(urb, pdoData);

                break;
            case USB_INTERFACE_DESCRIPTOR_TYPE:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> USB_INTERFACE_DESCRIPTOR_TYPE");

                break;

            case USB_ENDPOINT_DESCRIPTOR_TYPE:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> USB_ENDPOINT_DESCRIPTOR_TYPE");

                break;

            default:

                TraceEvents(TRACE_LEVEL_VERBOSE,
                    TRACE_BUSPDO,
                    ">> >> >> Unknown descriptor type");

                break;
            }

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                "<< <<");

            break;

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_GET_STATUS_FROM_DEVICE");

            // Defaults always succeed
            status = STATUS_SUCCESS;

            break;

        case URB_FUNCTION_ABORT_PIPE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_ABORT_PIPE");

            status = UsbPdo_AbortPipe(hDevice);

            break;

        case URB_FUNCTION_CLASS_INTERFACE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_CLASS_INTERFACE");

            status = UsbPdo_ClassInterface(urb, hDevice, pdoData);

            break;

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >> URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE");

            status = UsbPdo_GetDescriptorFromInterface(urb, pdoData);

            //
            // The DS4 is basically ready to operate at this stage
            // 
            if (pdoData->TargetType == DualShock4Wired)
            {
                //
                // Report back to FDO that we are ready to operate
                // 
                BUS_PDO_REPORT_STAGE_RESULT(
                    pdoData->BusInterface,
                    ViGEmPdoInitFinished,
                    pdoData->SerialNo,
                    STATUS_SUCCESS
                );
            }

            break;

        default:

            TraceEvents(TRACE_LEVEL_VERBOSE,
                TRACE_BUSPDO,
                ">> >>  Unknown function: 0x%X",
                urb->UrbHeader.Function);

            break;
        }

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            "<<");

        break;

    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            ">> IOCTL_INTERNAL_USB_GET_PORT_STATUS");

        // We report the (virtual) port as always active
        *(unsigned long *)irpStack->Parameters.Others.Argument1 = USBD_PORT_ENABLED | USBD_PORT_CONNECTED;

        status = STATUS_SUCCESS;

        break;

    case IOCTL_INTERNAL_USB_RESET_PORT:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            ">> IOCTL_INTERNAL_USB_RESET_PORT");

        // Sure, why not ;)
        status = STATUS_SUCCESS;

        break;

    case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            ">> IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION");

        // TODO: implement
        // This happens if the I/O latency is too high so HIDUSB aborts communication.
        status = STATUS_SUCCESS;

        break;

    default:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_BUSPDO,
            ">> Unknown I/O control code 0x%X",
            IoControlCode);

        break;
    }

    if (status != STATUS_PENDING)
    {
        WdfRequestComplete(Request, status);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_BUSPDO, "%!FUNC! Exit with status %!STATUS!", status);
}

