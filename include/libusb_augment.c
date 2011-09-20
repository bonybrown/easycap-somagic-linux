// 2005-10-19/lindi: downloaded from http://www.gaesi.org/~nmct/cvista/cvista/

// libusb_augment.c
// $Revision$
// $Date$

// Hopefully, the functions in this file will become part of libusb.

#include <stdio.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/time.h>
#include <usb.h> // lindi added
#include <linux/usbdevice_fs.h> // lindi added
#include <string.h> // lindi added
#define LIBUSB_AUGMENT
#include "libusb_augment.h"

// Taken from libusb file linux.h to provide macro definitions.
#define USB_URB_ISO_ASAP	2
#define USB_URB_TYPE_ISO	0
#define IOCTL_USB_SUBMITURB	_IOR('U', 10, struct usb_urb) // lindi changed "U" -> 'U'
#define IOCTL_USB_DISCARDURB	_IO('U', 11)
#define IOCTL_USB_REAPURBNDELAY	_IOW('U', 13, void *)

// Taken from libusb file usbi.h because usb.h
// hides the definition of usb_dev_handle.
extern int usb_debug;

struct usb_dev_handle {
  int fd;

  struct usb_bus *bus;
  struct usb_device *device;

  int config;
  int interface;
  int altsetting;

  /* Added by RMT so implementations can store other per-open-device data */
  void *impl_info;
};

// Taken from libusb file error.h to supply error handling macro definition.
typedef enum {
  USB_ERROR_TYPE_NONE = 0,
  USB_ERROR_TYPE_STRING,
  USB_ERROR_TYPE_ERRNO,
} usb_error_type_t;

extern char usb_error_str[1024];
extern usb_error_type_t usb_error_type;

#define USB_ERROR_STR(x, format, args...) \
	do { \
	  usb_error_type = USB_ERROR_TYPE_STRING; \
	  snprintf(usb_error_str, sizeof(usb_error_str) - 1, format, ## args); \
          if (usb_debug >= 2) \
            fprintf(stderr, "USB error: %s\n", usb_error_str); \
	  return x; \
	} while (0)

// Reading and writing are the same except for the endpoint
int usb_isochronous_setup(usb_urb **iso_urb, // URB pointer-pointer.
			  unsigned char ep,  // Device endpoint.
			  int pktsize,       // Endpoint packet size.
			  char *bytes,       // Data buffer pointer.
			  int size) {        // Size of the buffer.
  struct usb_urb *local_urb;
  // int ret
  // was unused /lindi
  int pktcount, fullpkts, partpktsize, packets, urb_size;

  // No more than 32768 bytes can be transferred at a time.
//  if (size > 32768) {
//    USB_ERROR_STR(-errno, "error on transfer size: %s", strerror(EINVAL));
//    return -EINVAL;
//  }

  // Determine the number of packets that need to be created based upon the
  // amount of data to be transferred, and the maximum packet size of the
  // endpoint.

  // Find integral number of full packets.
  //fprintf(stderr, "buf size: %d\n", size);
  //fprintf(stderr, "iso size: %d\n", pktsize);
  fullpkts = size / pktsize;
  //fprintf(stderr, "Number of full packets: %d\n", fullpkts);
  // Find length of partial packet.
  partpktsize = size % pktsize;
  //fprintf(stderr, "Size of partial packet: %d\n", partpktsize);
  // Find total number of packets to be transfered.
  packets = fullpkts + ((partpktsize > 0) ? 1 : 0);
  //fprintf(stderr, "Total number of packets: %d\n", packets);
  // Limit the number of packets transfered according to
  // the Linux usbdevfs maximum read/write buffer size.
  if ((packets < 1) || (packets > 128)) {
    USB_ERROR_STR(-errno, "error on packet size: %s", strerror(EINVAL));
    return -EINVAL;
  }

  // If necessary, allocate the urb and packet
  // descriptor structures from the heap.
  local_urb = *iso_urb;
  if (!local_urb) {
    urb_size = sizeof(struct usb_urb) + 
      packets * sizeof(struct usb_iso_packet_desc);
    local_urb = (struct usb_urb *) calloc(1, urb_size);
    if (!local_urb) {
      USB_ERROR_STR(-errno, "error on packet size: %s", strerror(EINVAL));
      return -ENOMEM;
    }
  }

  // Set up each packet for the data to be transferred.
  for (pktcount = 0; pktcount < fullpkts; pktcount++) {
    local_urb->iso_frame_desc[pktcount].length = pktsize;
    local_urb->iso_frame_desc[pktcount].actual_length = 0;
    local_urb->iso_frame_desc[pktcount].status = 0;
  }

  // Set up the last packet for the partial data to be transferred.
  if (partpktsize > 0) {
    local_urb->iso_frame_desc[pktcount].length = partpktsize;
    local_urb->iso_frame_desc[pktcount].actual_length = 0;
    local_urb->iso_frame_desc[pktcount++].status = 0;
  }

  // Set up the URB structure.
  local_urb->type = USB_URB_TYPE_ISO;
  //fprintf(stderr, "type: %d\n", local_urb->type);
  local_urb->endpoint = ep;
  //fprintf(stderr, "endpoint: 0x%x\n", local_urb->endpoint);
  local_urb->status = 0;
  local_urb->flags = USB_URB_ISO_ASAP; // Additional flags here?
  //fprintf(stderr, "flags: %d\n", local_urb->flags);
  local_urb->buffer = bytes;
  //fprintf(stderr, "buffer: 0x%x\n", local_urb->buffer);
  local_urb->buffer_length = size;
  //fprintf(stderr, "buffer_length: %d\n", local_urb->buffer_length);
  local_urb->actual_length = 0;
  local_urb->start_frame = 0;
  //fprintf(stderr, "start_frame: %d\n", local_urb->start_frame);
  local_urb->number_of_packets = pktcount;
  //fprintf(stderr, "number_of_packets: %d\n", local_urb->number_of_packets);
  local_urb->error_count = 0;
  local_urb->signr = 0;
  //fprintf(stderr, "signr: %d\n", local_urb->signr);
  local_urb->usercontext = (void *) 0;
  *iso_urb = local_urb;
  return 0;
}


int usb_isochronous_submit(usb_dev_handle *dev,     // Open usb device handle.
			   usb_urb *iso_urb,        // Pointer to URB.
			   struct timeval *tv_submit) { // Time structure pointer.
  int ret;

  // Get actual time, of the URB submission.
  gettimeofday(tv_submit, NULL);
  // Submit the URB through an IOCTL call.
  ret = ioctl(dev->fd, IOCTL_USB_SUBMITURB, iso_urb);
  //fprintf(stderr, "start_frame now: %d\n", iso_urb->start_frame);
  //fprintf(stderr, "submit ioctl return value: %d\n", ret);
  if (ret < 0) {
    //fprintf(stderr, "error submitting URB: %s\n", strerror(errno));
    USB_ERROR_STR(-errno, "error submitting URB: %s", strerror(errno));
  }
  return ret;
}


int usb_isochronous_reap(usb_dev_handle *dev,     // Open usb device handle.
			 usb_urb *iso_urb,        // Pointer to URB.
			 struct timeval *tv_reap, // Time structure pointer.
			 int timeout) {           // Attempt timeout (msec).
  struct timeval tv_ref, tv;
  void *context;
  int waiting, ret;

  // Get actual time, and add the timeout value. The result is the absolute
  // time where we have to quit waiting for an isochronous message.
  gettimeofday(&tv_ref, NULL);
  tv_ref.tv_sec = tv_ref.tv_sec + timeout / 1000;
  tv_ref.tv_usec = tv_ref.tv_usec + (timeout % 1000) * 1000;
  // Roll over 1e6 microseconds to one second.
  if (tv_ref.tv_usec > 1e6) {
    tv_ref.tv_usec -= 1e6;
    tv_ref.tv_sec++;
  }

  waiting = 1;
  //fprintf(stderr, "preparing to reap\n");
  while (((ret = ioctl(dev->fd, IOCTL_USB_REAPURBNDELAY, &context)) == -1) \
	 && waiting) {
    tv.tv_sec = 0;
    tv.tv_usec = 1000; // 1 msec
//    select(0, NULL, NULL, NULL, &tv); //sub second wait

    /* compare to actual time, as the select timeout isn"t that precise */
    gettimeofday(tv_reap, NULL);

    if ((tv_reap->tv_sec >= tv_ref.tv_sec) &&
	(tv_reap->tv_usec >= tv_ref.tv_usec))
      waiting = 0;
  }

  // Get actual time of the URB reap for maintaining data flow.
  gettimeofday(tv_reap, NULL);
  /*
   * If there was an error, that wasn"t EAGAIN (no completion), then
   * something happened during the reaping and we should return that
   * error now
   */
  //fprintf(stderr, "reap ioctl return value: %d\n", ret);
  if (ret < 0 && errno != EAGAIN) {
    USB_ERROR_STR(-errno, "error reaping interrupt URB: %s",
		  strerror(errno));
  }

  //fprintf(stderr, "actual_length: %d\n", iso_urb->actual_length);
  //fprintf(stderr, "URB status: %d\n", iso_urb->status);
  //fprintf(stderr, "error count: %d\n", iso_urb->error_count);

  //fprintf(stderr, "waiting done\n");

  // If the URB didn"t complete in success or error, then let"s unlink it.
  if (ret < 0) {
    int rc;

    if (!waiting)
      rc = -ETIMEDOUT;
    else
      rc = iso_urb->status;

    ret = ioctl(dev->fd, IOCTL_USB_DISCARDURB, iso_urb);
    //fprintf(stderr, "discard ioctl return value: %d\n", ret);
    if (ret < 0 && errno != EINVAL && usb_debug >= 1) {
      USB_ERROR_STR(-errno,
		    "error discarding isochronous URB: %s", strerror(errno));
    }
    //fprintf(stderr, "status: %d\n", rc);
    return rc;
  }

  //fprintf(stderr, "Total bytes: %d\n", bytesdone);
  return iso_urb->actual_length;
}


