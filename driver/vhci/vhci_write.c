#include "vhci.h"

#include "usbip_proto.h"
#include "usbreq.h"
#include "usbd_helper.h"

extern struct urb_req *
find_sent_urbr(pusbip_vpdo_dev_t vpdo, struct usbip_header *hdr);

static BOOLEAN
save_iso_desc(struct _URB_ISOCH_TRANSFER *urb, struct usbip_iso_packet_descriptor *iso_desc)
{
	ULONG	i;

	for (i = 0; i < urb->NumberOfPackets; i++) {
		if (iso_desc->offset > urb->IsoPacket[i].Offset) {
			DBGW(DBG_WRITE, "why offset changed?%d %d %d %d\n",
			     i, iso_desc->offset, iso_desc->actual_length, urb->IsoPacket[i].Offset);
			return FALSE;
		}
		urb->IsoPacket[i].Length = iso_desc->actual_length;
		urb->IsoPacket[i].Status = to_usbd_status(iso_desc->status);
		iso_desc++;
	}
	return TRUE;
}

static PVOID
get_buf(PVOID buf, PMDL bufMDL)
{
	if (buf == NULL) {
		if (bufMDL != NULL)
			buf = MmGetSystemAddressForMdlSafe(bufMDL, NormalPagePriority);
		if (buf == NULL) {
			DBGW(DBG_WRITE, "No transfer buffer\n");
		}
	}
	return buf;
}

static void
copy_iso_data(char *dest, ULONG dest_len, char *src, ULONG src_len, struct _URB_ISOCH_TRANSFER *urb)
{
	ULONG	i;
	ULONG	offset = 0;

	for (i = 0; i < urb->NumberOfPackets; i++) {
		if (urb->IsoPacket[i].Length == 0)
			continue;

		if (urb->IsoPacket[i].Offset + urb->IsoPacket[i].Length	> dest_len) {
			DBGW(DBG_WRITE, "Warning, why this?");
			break;
		}
		if (offset + urb->IsoPacket[i].Length > src_len) {
			DBGW(DBG_WRITE, "Warning, why that?");
			break;
		}
		RtlCopyMemory(dest + urb->IsoPacket[i].Offset, src + offset, urb->IsoPacket[i].Length);
		offset += urb->IsoPacket[i].Length;
	}
	if (offset != src_len) {
		DBGW(DBG_WRITE, "why not equal offset:%d src_len:%d", offset, src_len);
	}
}

static NTSTATUS
post_select_config(pusbip_vpdo_dev_t vpdo, PURB urb)
{
	PUSB_CONFIGURATION_DESCRIPTOR	dsc_conf;
	USHORT	len;
	struct _URB_SELECT_CONFIGURATION	*urb_selc = &urb->UrbSelectConfiguration;

	// If ConfigurationDescriptor is NULL, the device will be set to an unconfigured state.
	if (urb_selc->ConfigurationDescriptor == NULL) {
		DBGE(DBG_WRITE, "post_select_config: unconfigured state\n");
		return STATUS_SUCCESS;
	}

	len = urb_selc->ConfigurationDescriptor->wTotalLength;
	dsc_conf = ExAllocatePoolWithTag(NonPagedPool, len, USBIP_VHCI_POOL_TAG);
	if (dsc_conf == NULL) {
		DBGE(DBG_WRITE, "post_select_config: out of memory\n");
		return STATUS_UNSUCCESSFUL;
	}

	RtlCopyMemory(dsc_conf, urb_selc->ConfigurationDescriptor, len);
	if (vpdo->dsc_conf)
		ExFreePoolWithTag(vpdo->dsc_conf, USBIP_VHCI_POOL_TAG);
	vpdo->dsc_conf = dsc_conf;

	return select_config(urb_selc, vpdo->speed);
}

static NTSTATUS
post_select_interface(pusbip_vpdo_dev_t vpdo, PURB urb)
{
	struct _URB_SELECT_INTERFACE	*urb_seli = &urb->UrbSelectInterface;

	if (vpdo->dsc_conf == NULL) {
		DBGW(DBG_WRITE, "post_select_interface: empty configuration descriptor\n");
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	return select_interface(urb_seli, vpdo->dsc_conf, vpdo->speed);
}

static NTSTATUS
copy_to_transfer_buffer(PVOID buf_dst, PMDL bufMDL, int dst_len, PVOID src, int src_len)
{
	PVOID	buf;

	if (dst_len < src_len) {
		DBGE(DBG_WRITE, "too small buffer: dest: %d, src: %d\n", dst_len, src_len);
		return STATUS_INVALID_PARAMETER;
	}
	buf = get_buf(buf_dst, bufMDL);
	if (buf == NULL)
		return STATUS_INVALID_PARAMETER;

	RtlCopyMemory(buf, src, src_len);
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_get_desc(PURB urb, struct usbip_header *hdr)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST *urb_desc = &urb->UrbControlDescriptorRequest;
	NTSTATUS	status;

	status = copy_to_transfer_buffer(urb_desc->TransferBuffer, urb_desc->TransferBufferMDL,
		urb_desc->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
	if (status == STATUS_SUCCESS)
		urb_desc->TransferBufferLength = hdr->u.ret_submit.actual_length;
	return status;
}

static NTSTATUS
store_urb_get_status(PURB urb, struct usbip_header *hdr)
{
	struct _URB_CONTROL_GET_STATUS_REQUEST	*urb_gsr = &urb->UrbControlGetStatusRequest;
	NTSTATUS	status;

	status = copy_to_transfer_buffer(urb_gsr->TransferBuffer, urb_gsr->TransferBufferMDL,
		urb_gsr->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
	if (status == STATUS_SUCCESS)
		urb_gsr->TransferBufferLength = hdr->u.ret_submit.actual_length;
	return status;
}

static NTSTATUS
store_urb_control_transfer(PURB urb, struct usbip_header *hdr)
{
	struct _URB_CONTROL_TRANSFER	*urb_desc = &urb->UrbControlTransfer;
	NTSTATUS	status;

	if (urb_desc->TransferBufferLength == 0)
		return STATUS_SUCCESS;
	status = copy_to_transfer_buffer(urb_desc->TransferBuffer, urb_desc->TransferBufferMDL,
		urb_desc->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
	if (status == STATUS_SUCCESS)
		urb_desc->TransferBufferLength = hdr->u.ret_submit.actual_length;
	return status;
}

static NTSTATUS
store_urb_control_transfer_ex(PURB urb, struct usbip_header* hdr)
{
	struct _URB_CONTROL_TRANSFER_EX	*urb_desc = &urb->UrbControlTransferEx;
	NTSTATUS	status;

	status = copy_to_transfer_buffer(urb_desc->TransferBuffer, urb_desc->TransferBufferMDL,
		urb_desc->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
	if (status == STATUS_SUCCESS)
		urb_desc->TransferBufferLength = hdr->u.ret_submit.actual_length;
	return status;
}

static NTSTATUS
store_urb_vendor_or_class(PURB urb, struct usbip_header *hdr)
{
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST	*urb_vendor_class = &urb->UrbControlVendorClassRequest;

	if (urb_vendor_class->TransferFlags & USBD_TRANSFER_DIRECTION_IN) {
		NTSTATUS	status;
		status = copy_to_transfer_buffer(urb_vendor_class->TransferBuffer, urb_vendor_class->TransferBufferMDL,
			urb_vendor_class->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
		if (status == STATUS_SUCCESS)
			urb_vendor_class->TransferBufferLength = hdr->u.ret_submit.actual_length;
		return status;
	}
	else {
		urb_vendor_class->TransferBufferLength = hdr->u.ret_submit.actual_length;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_bulk_or_interrupt(PURB urb, struct usbip_header *hdr)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER	*urb_bi = &urb->UrbBulkOrInterruptTransfer;

	if (PIPE2DIRECT(urb_bi->PipeHandle)) {
		NTSTATUS	status;
		status = copy_to_transfer_buffer(urb_bi->TransferBuffer, urb_bi->TransferBufferMDL,
			urb_bi->TransferBufferLength, hdr + 1, hdr->u.ret_submit.actual_length);
		if (status == STATUS_SUCCESS)
			urb_bi->TransferBufferLength = hdr->u.ret_submit.actual_length;
		return status;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_iso(PURB urb, struct usbip_header *hdr)
{
	struct _URB_ISOCH_TRANSFER	*urb_iso = &urb->UrbIsochronousTransfer;
	struct usbip_iso_packet_descriptor	*iso_desc;
	PVOID	buf;
	int	in_len = 0;

	if (PIPE2DIRECT(urb_iso->PipeHandle))
		in_len = hdr->u.ret_submit.actual_length;
	iso_desc = (struct usbip_iso_packet_descriptor *)((char *)(hdr + 1) + in_len);
	if (!save_iso_desc(urb_iso, iso_desc))
		return STATUS_INVALID_PARAMETER;

	urb_iso->ErrorCount = hdr->u.ret_submit.error_count;
	buf = get_buf(urb_iso->TransferBuffer, urb_iso->TransferBufferMDL);
	if (buf == NULL)
		return STATUS_INVALID_PARAMETER;
	copy_iso_data(buf, urb_iso->TransferBufferLength, (char *)(hdr + 1), hdr->u.ret_submit.actual_length, urb_iso);
	urb_iso->TransferBufferLength = hdr->u.ret_submit.actual_length;
	return STATUS_SUCCESS;
}

static NTSTATUS
store_urb_data(PURB urb, struct usbip_header *hdr)
{
	NTSTATUS	status;

	switch (urb->UrbHeader.Function) {
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
	case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
		status = store_urb_get_desc(urb, hdr);
		break;
	case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
	case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
	case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
	case URB_FUNCTION_GET_STATUS_FROM_OTHER:
		status = store_urb_get_status(urb, hdr);
		break;
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
	case URB_FUNCTION_VENDOR_OTHER:
		status = store_urb_vendor_or_class(urb, hdr);
		break;
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
		status = store_urb_bulk_or_interrupt(urb, hdr);
		break;
	case URB_FUNCTION_ISOCH_TRANSFER:
		status = store_urb_iso(urb, hdr);
		break;
	case URB_FUNCTION_SELECT_CONFIGURATION:
		status = STATUS_SUCCESS;
		break;
	case URB_FUNCTION_SELECT_INTERFACE:
		status = STATUS_SUCCESS;
		break;
	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		status = STATUS_SUCCESS;
		break;
	case URB_FUNCTION_CONTROL_TRANSFER:
		status = store_urb_control_transfer(urb, hdr);
		break;
	case URB_FUNCTION_CONTROL_TRANSFER_EX:
		status = store_urb_control_transfer_ex(urb, hdr);
		break;
	default:
		DBGE(DBG_WRITE, "not supported func: %s\n", dbg_urbfunc(urb->UrbHeader.Function));
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	if (status == STATUS_SUCCESS)
		urb->UrbHeader.Status = to_usbd_status(hdr->u.ret_submit.status);

	return status;
}

static NTSTATUS
process_urb_res_submit(pusbip_vpdo_dev_t vpdo, PURB urb, struct usbip_header *hdr)
{
	NTSTATUS	status;

	if (urb == NULL)
		return STATUS_INVALID_PARAMETER;

	if (hdr->u.ret_submit.status != 0) {
		urb->UrbHeader.Status = to_usbd_status(hdr->u.ret_submit.status);
		if (urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
			urb->UrbBulkOrInterruptTransfer.TransferBufferLength = hdr->u.ret_submit.actual_length;
		}
		DBGW(DBG_WRITE, "%s: wrong status: %s\n", dbg_urbfunc(urb->UrbHeader.Function), dbg_usbd_status(urb->UrbHeader.Status));
		return STATUS_UNSUCCESSFUL;
	}

	status = store_urb_data(urb, hdr);
	if (status == STATUS_SUCCESS) {
		switch (urb->UrbHeader.Function) {
		case URB_FUNCTION_SELECT_CONFIGURATION:
			status = post_select_config(vpdo, urb);
			break;
		case URB_FUNCTION_SELECT_INTERFACE:
			status = post_select_interface(vpdo, urb);
			break;
		default:
			break;
		}
	}
	return status;
}

static NTSTATUS
process_urb_res(struct urb_req *urbr, struct usbip_header *hdr)
{
	PIO_STACK_LOCATION	irpstack;
	ULONG	ioctl_code;

	if (urbr->irp == NULL)
		return STATUS_SUCCESS;

	irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	ioctl_code = irpstack->Parameters.DeviceIoControl.IoControlCode;

	DBGI(DBG_WRITE, "process_urb_res: urbr:%s, ioctl:%s\n", dbg_urbr(urbr), dbg_vhci_ioctl_code(ioctl_code));

	switch (ioctl_code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		return process_urb_res_submit(urbr->vpdo, irpstack->Parameters.Others.Argument1, hdr);
	case IOCTL_INTERNAL_USB_RESET_PORT:
		return STATUS_SUCCESS;
	default:
		DBGE(DBG_WRITE, "unhandled ioctl: %s\n", dbg_vhci_ioctl_code(ioctl_code));
		return STATUS_INVALID_PARAMETER;
	}
}

static struct usbip_header *
get_usbip_hdr_from_write_irp(PIRP irp)
{
	PIO_STACK_LOCATION	irpstack;
	ULONG	len;

	irpstack = IoGetCurrentIrpStackLocation(irp);
	len = irpstack->Parameters.Write.Length;
	if (len < sizeof(struct usbip_header)) {
		return NULL;
	}
	irp->IoStatus.Information = len;
	return (struct usbip_header *)irp->AssociatedIrp.SystemBuffer;
}

static NTSTATUS
process_write_irp(pusbip_vpdo_dev_t vpdo, PIRP write_irp)
{
	struct usbip_header	*hdr;
	struct urb_req	*urbr;
	KIRQL	oldirql;
	NTSTATUS	status;

	hdr = get_usbip_hdr_from_write_irp(write_irp);
	if (hdr == NULL) {
		DBGE(DBG_WRITE, "small write irp\n");
		return STATUS_INVALID_PARAMETER;
	}

	urbr = find_sent_urbr(vpdo, hdr);
	if (urbr == NULL) {
		// Might have been cancelled before, so return STATUS_SUCCESS
		DBGE(DBG_WRITE, "no urbr: seqnum: %d\n", hdr->base.seqnum);
		return STATUS_SUCCESS;
	}

	status = process_urb_res(urbr, hdr);
	PIRP irp = urbr->irp;
	free_urbr(urbr);

	if (irp != NULL) {
		BOOLEAN valid_irp;
		IoAcquireCancelSpinLock(&oldirql);
		valid_irp = IoSetCancelRoutine(irp, NULL) != NULL;
		IoReleaseCancelSpinLock(oldirql);
		if (valid_irp) {
			irp->IoStatus.Status = status;

			/* it seems windows client usb driver will think
			 * IoCompleteRequest is running at DISPATCH_LEVEL
			 * so without this it will change IRQL sometimes,
			 * and introduce to a dead of my userspace program
			 */
			KeRaiseIrql(DISPATCH_LEVEL, &oldirql);
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			KeLowerIrql(oldirql);
		}
	}

	return STATUS_SUCCESS;
}

PAGEABLE NTSTATUS
vhci_write(__in PDEVICE_OBJECT devobj, __in PIRP Irp)
{
	pusbip_vhub_dev_t	vhub;
	pusbip_vpdo_dev_t	vpdo;
	pdev_common_t		devcom;
	PIO_STACK_LOCATION	stackirp;
	NTSTATUS		status;

	PAGED_CODE();

	devcom = (pdev_common_t)devobj->DeviceExtension;

	DBGI(DBG_GENERAL | DBG_WRITE, "vhci_write: Enter\n");

	if (!devcom->is_vhub) {
		DBGE(DBG_WRITE, "write for vhub is not allowed\n");

		Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	vhub = (pusbip_vhub_dev_t)devobj->DeviceExtension;

	inc_io_vhub(vhub);

	if (vhub->common.DevicePnPState == Deleted) {
		status = STATUS_NO_SUCH_DEVICE;
		goto END;
	}
	stackirp = IoGetCurrentIrpStackLocation(Irp);
	vpdo = stackirp->FileObject->FsContext;
	if (vpdo == NULL || !vpdo->plugged) {
		status = STATUS_INVALID_DEVICE_REQUEST;
		goto END;
	}
	Irp->IoStatus.Information = 0;
	status = process_write_irp(vpdo, Irp);
END:
	DBGI(DBG_WRITE, "vhci_write: Leave: %s\n", dbg_ntstatus(status));
	Irp->IoStatus.Status = status;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	dec_io_vhub(vhub);

	return status;
}
