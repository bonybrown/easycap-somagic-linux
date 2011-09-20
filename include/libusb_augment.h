// libusb_augment.h
// $Revision$
// $Date$

#ifdef LIBUSB_AUGMENT
// Taken from libusb file linux.h to provide the URB structure definitions.
struct usb_iso_packet_desc {
  unsigned int length;
  unsigned int actual_length;
  unsigned int status;
};

struct usb_urb {
  unsigned char type;     // >CM
  unsigned char endpoint; // >CM
  int status;             // <C
  unsigned int flags;     // TCO
  void *buffer;           // >CM
  int buffer_length;      // >CM
  int actual_length;      // <C
  int start_frame;        // T-XX
  int number_of_packets;  // >--X
  int error_count;        // <--X
  unsigned int signr;     // signal to be sent on error.
                          // -1 if none should be sent (sic, try 0!)
  void *usercontext;      // >CO
  struct usbdevfs_iso_packet_desc iso_frame_desc[0];
};
#else
// Hide the definition of usb_urb from everyone else (TBR).
struct usb_urb;
#endif

typedef struct usb_urb usb_urb;

int usb_isochronous_setup(usb_urb **iso_urb, unsigned char ep,
			  int pktsize, char *bytes, int size);
int usb_isochronous_submit(usb_dev_handle *dev, usb_urb *iso_urb,
			   struct timeval *tv_rsubmit);
int usb_isochronous_reap(usb_dev_handle *dev, usb_urb *iso_urb,
			 struct timeval *tv_reap, int timeout);

