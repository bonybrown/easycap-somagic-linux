#!/usr/bin/perl -w
# usbsnoop2libusb.pl Convert usbsnoop logs into C programs that use libusb
# Copyright (C) 2005,2006,2007,2010  Timo Juhani Lindfors <timo.lindfors@iki.fi>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# This program was written mainly to learn USB and to write free
# drivers for USB devices. It is not of very high quality but any
# comments, bug reports or new ideas are welcome!
#
# Please see http://iki.fi/lindi/usb/usbsnoop.txt for usage instructions.
#
# 2009-03-05/lindi: moved to darcs, use "darcs log" to see changelog
# 2007-11-25/lindi: URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT
# 2006-01-07/lindi: ignore "non printable" URBs
# 2006-08-30/lindi: handle URB_FUNCTION_SELECT_INTERFACE, escape \0 in perl
# 2006-07-18/lindi: allow product id to be 0x0000
# 2006-03-09/lindi: don't fill buffer before reads
# 2005-09-18/lindi: parse transfer buffers properly
# 2005-12-24/lindi: 
use strict;

my ($line,$urb, $last_time);

my %endpointtype;

$last_time = 0;

# Note that if you enable support for isochronous transfers you also
# need the following two files:
# http://iki.fi/lindi/usb/libusb_augment.c
# http://iki.fi/lindi/usb/libusb_augment.h
my $useisoch = 1;

my $usbsnoopbugseen = 0;

sub get_transferbuffer_contents {
    my (@lines) = @_;
    my $buffer = "";
    foreach my $line (@lines) {
	chomp($line);
	if ($line =~ /^    0[0-9a-f]{7}: ([^ ].+)/) {
	    my $bytes = $1;
	    $bytes =~ s/ /\\x/g;
	    $bytes =~ s/^/\\x/;
	    $buffer .= $bytes;
	}
    }
    return $buffer;
}

sub process_urb {
    my $text = shift;
    if (!defined($text)) {
	return;
    }
    my @lines = split(/\n/, $text);
    if ($text =~ m/UsbSnoop - incorrect UrbHeader.Length/) {
	print("/* usbsnoop bug detected */\n");
	$usbsnoopbugseen = 1;
    }
    if ($text =~ m/coming back/) {
	if ($text =~ m/URB_FUNCTION_SELECT_CONFIGURATION/) {
	    my $endpoint;
	    foreach my $line (@lines) {
		if ($line =~ /EndpointAddress *= 0x([a-f0-9]+)/) {
		    $endpoint = $1;
		}
		if ($line =~ /PipeType *= 0x([a-f0-9]+) /) {
		    my $pipetype = $1;
		    $endpointtype{$endpoint} = $pipetype;
		}
	    }
	} elsif (($usbsnoopbugseen == 1) and ($text =~ m/URB_FUNCTION_SELECT_INTERFACE/)) {
	    # UsbSnoop - incorrect UrbHeader.Length=0, should be at least 16
	    # and usbsnoop does not show what we sent! So, we need to parse
	    # alternate interface from the reply.
	    if ($text =~ m/Interface: AlternateSetting *= ([0-9]+)/) {
		my $AlternateSetting = $1;
		chomp($AlternateSetting);
		if ($text =~ /URB (\d+) /) {
		    my $urbnumber = $1;
		    chomp($urbnumber);
		    print "ret = usb_set_altinterface(devh, $AlternateSetting);\n";
		    print "printf(\"$urbnumber set alternate setting returned %d\\n\", ret);\n";
		}
	    }
	    $usbsnoopbugseen = 0;
	}
	return;
    }
#    print "text=\"$text\"\n";
    my ($time, $TransferBufferLength, $DescriptorType, $Index, $bConfigurationValue, $Value, $Request, $endpoint, $urbnumber, $AlternateSetting, $FeatureSelector, $IsoPacket1Offset, $IsoPacketLastIndex);
    if ($text =~ m/(\d+) ms/) {
	$time = $1;
    }

    if ($last_time == 0) {
	$last_time = $time;
    } else {
	my $timediff = $time - $last_time;
	if ($timediff > 2500) {
	    $timediff = 2500; # FIXME
	}
	if ($timediff > 0) {
	    print "usleep($timediff*1000);\n";
	}
	$last_time = $time;
    }
    if ($text =~ m/TransferBufferLength = 0([^ ]+)/) {
	$TransferBufferLength = $1;
	chomp($TransferBufferLength);
    }
    if ($text =~ m/DescriptorType *= 0([^ ]+)/) {
	$DescriptorType = $1;
	chomp($DescriptorType);
    }
    if ($text =~ m/Index *= 0([a-f0-9]+)/) {
	$Index = $1;
	chomp($Index);
    }
    if ($text =~ m/bConfigurationValue *= 0x0([^ ]+)/) {
	$bConfigurationValue = $1;
	chomp($bConfigurationValue);
    }
    if ($text =~ m/Value *= 0([^ ]+)/) {
	$Value = $1;
	chomp($Value);
    }
    if ($text =~ m/Request *= 0([^ ]+)/) {
	$Request = $1;
	chomp($Request);
    }
    if ($text =~ m/endpoint 0x([^ \]]+)/) {
	$endpoint = $1;
	chomp($endpoint);
    }
    if ($text =~ m/URB (\d+) /) {
	$urbnumber = $1;
	chomp($urbnumber);
    }
    if ($text =~ m/AlternateSetting *= ([0-9]+)/) {
	$AlternateSetting = $1;
	chomp($AlternateSetting);
    }
    if ($text =~ m/FeatureSelector *= ([0-9]+)/) {
	$FeatureSelector = $1;
	chomp($FeatureSelector);
    }
    if ($text =~ m/IsoPacket\[1\].Offset *= (\d+)/) {
	$IsoPacket1Offset = $1;
	chomp($IsoPacket1Offset);
    }
    foreach my $line (@lines) {
	if ($line =~ /IsoPacket\[(\d+)\]/) {
	    $IsoPacketLastIndex = $1;
	    chomp($IsoPacketLastIndex);
	}
    }
    if ($text =~ m/URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE/) {
	print "ret = usb_get_descriptor(devh, 0x$DescriptorType, 0x$Index, buf, 0x$TransferBufferLength);\n";
	print "printf(\"$urbnumber get descriptor returned %d, bytes: \", ret);\n";
	print "print_bytes(buf, ret);\n";
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE/) {
	printf "ret = usb_get_descriptor(devh, 0x$DescriptorType, 0x$Index, buf, 0x$TransferBufferLength);\n";
	print "printf(\"$urbnumber get descriptor returned %d, bytes: \", ret);\n";
	print "print_bytes(buf, ret);\n";
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_SELECT_CONFIGURATION/) {
	if (defined($bConfigurationValue)) {
	    print "ret = usb_release_interface(devh, 0);\n";
	    print "if (ret != 0) printf(\"failed to release interface before set_configuration: %d\\n\", ret);\n";
	    print "ret = usb_set_configuration(devh, 0x$bConfigurationValue);\n";
	    print "printf(\"$urbnumber set configuration returned %d\\n\", ret);\n";
	    print "ret = usb_claim_interface(devh, 0);\n";
	    print "if (ret != 0) printf(\"claim after set_configuration failed with error %d\\n\", ret);\n";
	}
	if (defined($AlternateSetting)) {
	    print "ret = usb_set_altinterface(devh, $AlternateSetting);\n";
	    print "printf(\"$urbnumber set alternate setting returned %d\\n\", ret);\n";
	}    
    } elsif ($text =~ m/URB_FUNCTION_CLASS_INTERFACE/) {
	my $requesttype = "USB_TYPE_CLASS + USB_RECIP_INTERFACE";
	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    $requesttype .= " + USB_ENDPOINT_IN";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	print "ret = usb_control_msg(devh, $requesttype, 0x$Request, 0x$Value, 0x$Index, buf, 0x$TransferBufferLength, 1000);\n";

	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    print "printf(\"$urbnumber control msg returned %d, bytes: \", ret);\nprint_bytes(buf, ret);\n";
	} else {
	    print "printf(\"$urbnumber control msg returned %d\", ret);\n";
	}
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_CLASS_ENDPOINT/) {
	my $requesttype = "USB_TYPE_CLASS + USB_RECIP_ENDPOINT";
	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    $requesttype .= " + USB_ENDPOINT_IN";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	print "ret = usb_control_msg(devh, $requesttype, 0x$Request, 0x$Value, 0x$Index, buf, 0x$TransferBufferLength, 1000);\n";

	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    print "printf(\"$urbnumber control msg returned %d, bytes: \", ret);\nprint_bytes(buf, ret);\n";
	} else {
	    print "printf(\"$urbnumber control msg returned %d\", ret);\n";
	}
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_CLASS_DEVICE/) {
	my $requesttype = "USB_TYPE_CLASS + USB_RECIP_DEVICE";
	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    $requesttype .= " + USB_ENDPOINT_IN";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	print "ret = usb_control_msg(devh, $requesttype, 0x$Request, 0x$Value, 0x$Index, buf, 0x$TransferBufferLength, 1000);\n";

	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    print "printf(\"$urbnumber control msg returned %d, bytes: \", ret);\nprint_bytes(buf, ret);\n";
	} else {
	    print "printf(\"$urbnumber control msg returned %d\", ret);\n";
	}
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_VENDOR_INTERFACE/) {
	my $requesttype = "USB_TYPE_VENDOR + USB_RECIP_INTERFACE";
	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    $requesttype .= " + USB_ENDPOINT_IN";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	print "ret = usb_control_msg(devh, $requesttype, 0x$Request, 0x$Value, 0x$Index, buf, 0x$TransferBufferLength, 1000);\n";
	print "printf(\"$urbnumber control msg returned %d, bytes: \", ret);\n";
	print "print_bytes(buf, ret);\n";
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER/) {
	my $method;
	if ( ( hex $endpoint & 128 ) == 128 ) {
	    # if ($text =~ /USBD_TRANSFER_DIRECTION_IN/) {
	    $method = "read";
	} else {
	    $method = "write";
	}
	my $type = $endpointtype{$endpoint};
	if (!defined($type)) {
	    die "can't find \"$endpoint\"\n";
	}
	my $mode;
	if ($type eq "00000002") {
	    $mode = "bulk";
	} elsif ($type eq "00000003") {
	    $mode = "interrupt";
	} else {
	    die "unrecognised endpointtype \"$type\" for endpoint \"$endpoint\"";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "" and $method eq "write") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	my $timeout = 1000;
	if ($mode eq "bulk" and hex($TransferBufferLength) > 50) {
	    $timeout = 1000 + hex($TransferBufferLength) * 0.06;
	    $timeout = sprintf("%.0d", $timeout); # round to integer
	}
	print "ret = usb_".$mode."_$method(devh, 0x$endpoint, buf, 0x$TransferBufferLength, $timeout);\n";
	print "printf(\"$urbnumber $mode $method returned %d, bytes: \", ret);\n";
# sounds weird but write requests can actually also return data
#	if ($method eq "read") {
	    print "print_bytes(buf, ret);\n";
#	}
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_SET_FEATURE_TO_DEVICE/) {
	my $requesttype = "USB_TYPE_STANDARD + USB_RECIP_DEVICE";
	print "ret = usb_control_msg(devh, $requesttype, USB_REQ_SET_FEATURE, $FeatureSelector, 0, buf, 0, 1000);\n";
	print "printf(\"$urbnumber set feature request returned %d\\n\", ret);\n";
    } elsif ($text =~ m/URB_FUNCTION_VENDOR_DEVICE/) {
	my $requesttype = "USB_TYPE_VENDOR + USB_RECIP_DEVICE";
	if ($text =~ m/USBD_TRANSFER_DIRECTION_IN/) {
	    $requesttype .= " + USB_ENDPOINT_IN";
	}
	my $bytes = get_transferbuffer_contents(@lines);
	if ($bytes ne "") {
	    print "memcpy(buf, \"$bytes\", 0x$TransferBufferLength);\n";
	}
	print "ret = usb_control_msg(devh, $requesttype, 0x$Request, 0x$Value, 0x$Index, buf, 0x$TransferBufferLength, 1000);\n";
	print "printf(\"$urbnumber control msg returned %d, bytes: \", ret);\n";
	print "print_bytes(buf, ret);\n";
	print "printf(\"\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_GET_CURRENT_FRAME_NUMBER/) {
	my $requesttype = "USB_TYPE_VENDOR + USB_RECIP_DEVICE";
	# TODO
    } elsif ($text =~ m/URB_FUNCTION_ISOCH_TRANSFER/) {
	if ($useisoch == 0) {
	    die "please set useisoch = 1 in the source code to enable experimental isochronous support";
	}
	if (!defined($IsoPacket1Offset)) {
	    die "can't find offset of first iso packet\n";
	}
	if (!defined($IsoPacketLastIndex)) {
	    die "can't find index of last iso packet\n";
	}
	my $packetsize = $IsoPacket1Offset;
	my $packetcount = $IsoPacketLastIndex + 1;
	if ($packetcount * $packetsize > 393216) {
	    die "packetcount ($packetcount) or packetsize ($packetsize) is way too large!\n";
	}
	print "ret = usb_isochronous_setup(&isourb, 0x$endpoint, $packetsize, isobuf, $packetcount * $packetsize);\n";
	print "printf(\"$urbnumber isochronous setup returned %d\\n\", ret);\n";
	print "ret = usb_isochronous_submit(devh, isourb, &isotv);\n";
	print "printf(\"$urbnumber isochronous submit returned %d\\n\", ret);\n";
	print "ret = usb_isochronous_reap(devh, isourb, &isotv, 1000);\n";
	print "printf(\"$urbnumber isochronous reap returned %d, bytes: \", ret);\n";
	print "print_bytes(isourb->buffer, ret);\n";
	print "printf(\"\\n\");\n";
	# TODO
    } elsif ($text =~ m/URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT/) {
	my $requesttype = "USB_TYPE_STANDARD + USB_RECIP_ENDPOINT";
	print "ret = usb_control_msg(devh, $requesttype, USB_REQ_CLEAR_FEATURE, $FeatureSelector, 0, buf, 0, 1000);\n";
	print "printf(\"$urbnumber clear feature request returned %d\\n\", ret);\n";
    } elsif ($text =~ m/URB_FUNCTION_ABORT_PIPE/) {
	# TODO: implement
	print "printf(\"$urbnumber abort pipe not implemented yet :(\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_RESET_PIPE/) {
	# TODO: implement
	print "printf(\"$urbnumber reset pipe not implemented yet :(\\n\");\n";
    } elsif ($text =~ m/URB_FUNCTION_SELECT_INTERFACE/) {
        if (!defined($AlternateSetting)) {
            die "can't find alternatesetting\n";
        }
        print "ret = usb_set_altinterface(devh, $AlternateSetting);\n";
        print "printf(\"$urbnumber set alternate setting returned %d\\n\", ret);\n";
    } elsif ($text =~ m/incorrect UrbHeader.Length=0,/) {
	# ignore
    } elsif ($text =~ m/non printable URB with function code 0x0000002a/) {
	print("/* usbsnoop says URB is non prinbable! */");
    } else {
	die "unrecognized URB type, text = \"$text\"";
    }
}


print <<EOF
/* This file is generated with usbsnoop2libusb.pl from a usbsnoop log file. */
/* Latest version of the script should be in http://iki.fi/lindi/usb/usbsnoop2libusb.pl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <ctype.h>
#include <usb.h>
#if $useisoch
 #include <linux/usbdevice_fs.h>
 #define LIBUSB_AUGMENT
 #include "libusb_augment.h"
#endif

struct usb_dev_handle *devh;

void release_usb_device(int dummy) {
    int ret;
    ret = usb_release_interface(devh, 0);
    if (!ret)
	printf("failed to release interface: %d\\n", ret);
    usb_close(devh);
    if (!ret)
	printf("failed to close interface: %d\\n", ret);
    exit(1);
}

void list_devices() {
    struct usb_bus *bus;
    for (bus = usb_get_busses(); bus; bus = bus->next) {
	struct usb_device *dev;
	
	for (dev = bus->devices; dev; dev = dev->next)
	    printf("0x%04x 0x%04x\\n",
		   dev->descriptor.idVendor,
		   dev->descriptor.idProduct);
    }
}    

struct usb_device *find_device(int vendor, int product) {
    struct usb_bus *bus;
    
    for (bus = usb_get_busses(); bus; bus = bus->next) {
	struct usb_device *dev;
	
	for (dev = bus->devices; dev; dev = dev->next) {
	    if (dev->descriptor.idVendor == vendor
		&& dev->descriptor.idProduct == product)
		return dev;
	}
    }
    return NULL;
}

void print_bytes(char *bytes, int len) {
    int i;
    if (len > 0) {
	for (i=0; i<len; i++) {
	    printf("%02x ", (int)((unsigned char)bytes[i]));
	}
	printf("\\"");
        for (i=0; i<len; i++) {
	    printf("%c", isprint(bytes[i]) ? bytes[i] : '.');
        }
        printf("\\"");
    }
}


int main(int argc, char **argv) {
    int ret, vendor, product;
    struct usb_device *dev;
    char buf[65535], *endptr;
#if $useisoch
    usb_urb *isourb;
    struct timeval isotv;
    char isobuf[393216];
#endif

    usb_init();
    usb_set_debug(255);
    usb_find_busses();
    usb_find_devices();

    if (argc!=3) {
	printf("usage: %s vendorID productID\\n", argv[0]);
	printf("ID numbers of currently attached devices:\\n");
	list_devices();
	exit(1);
    }
    vendor = strtol(argv[1], &endptr, 16);
    if (*endptr != '\\0') {
	printf("invalid vendor id\\n");
	exit(1);
    }
    product = strtol(argv[2], &endptr, 16);
    if (*endptr != '\\0') {
	printf("invalid product id\\n");
	exit(1);
    }
    dev = find_device(vendor, product);
    assert(dev);

    devh = usb_open(dev);
    assert(devh);
    
    signal(SIGTERM, release_usb_device);

    ret = usb_get_driver_np(devh, 0, buf, sizeof(buf));
    printf("usb_get_driver_np returned %d\\n", ret);
    if (ret == 0) {
	printf("interface 0 already claimed by driver \\"%s\\", attempting to detach it\\n", buf);
	ret = usb_detach_kernel_driver_np(devh, 0);
	printf("usb_detach_kernel_driver_np returned %d\\n", ret);
    }
    ret = usb_claim_interface(devh, 0);
    if (ret != 0) {
	printf("claim failed with error %d\\n", ret);
		exit(1);
    }
    
    ret = usb_set_altinterface(devh, 0);
    assert(ret >= 0);

EOF
    ;
    
while (defined($line = <>)) {
    if ($line =~ m/ URB (\d+) (going down|coming back)/) {
        &process_urb($urb);
        $urb = $line;
    } elsif (defined($urb)) {
        $urb .= $line;
    }
}

&process_urb($urb);

print <<EOF
	ret = usb_release_interface(devh, 0);
	assert(ret == 0);
	ret = usb_close(devh);
	assert(ret == 0);
	return 0;
}
EOF
    ;
