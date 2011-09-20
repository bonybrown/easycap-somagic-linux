/* This file is generated with usbsnoop2libusb.pl from a usbsnoop log file. */
/* Latest version of the script should be in http://iki.fi/lindi/usb/usbsnoop2libusb.pl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <ctype.h>
#include <libusb-1.0/libusb.h>

#define MIN(a, b)  (((a) < (b)) ? (a) : (b))


struct libusb_device_handle *devh;


void release_usb_device(int dummy) {
    fprintf( stderr, "Emergency exit\n");
    int ret;
    ret = libusb_release_interface(devh, 0);
    if (!ret)
	fprintf( stderr, "failed to release interface: %d\n", ret);
    libusb_close(devh);
    if (!ret)
	fprintf( stderr, "failed to close interface: %d\n", ret);
    libusb_exit(NULL);
    exit(1);
}


struct libusb_device *find_device(int vendor, int product) {
    /*struct usb_bus *bus;
    
    for (bus = usb_get_busses(); bus; bus = bus->next) {
	struct usb_device *dev;
	
	for (dev = bus->devices; dev; dev = dev->next) {
	    if (dev->descriptor.idVendor == vendor
		&& dev->descriptor.idProduct == product)
		return dev;
	}
    }
    return NULL;*/
    struct libusb_device **list;
    struct libusb_device *dev = NULL;
    struct libusb_device_descriptor descriptor;
    ssize_t count;
    count = libusb_get_device_list(NULL, &list);
    int i;
    for( i = 0; i < count; i++ )
    {
      struct libusb_device *item = list[i];
      libusb_get_device_descriptor( item, &descriptor );
      if (descriptor.idVendor == vendor
		&& descriptor.idProduct == product)
      {
	dev = item;
      }
      else
      {
	libusb_unref_device( item );
      }
    }
    libusb_free_device_list( list, 0 );
    return dev;
}

void print_bytes(char *bytes, int len) {
    int i;
    if (len > 0) {
	for (i=0; i<len; i++) {
	    fprintf( stderr, "%02x ", (int)((unsigned char)bytes[i]));
	}
	fprintf( stderr, "\"");
        for (i=0; i<len; i++) {
	    fprintf( stderr, "%c", isprint(bytes[i]) ? bytes[i] : '.');
        }
        fprintf( stderr, "\"");
    }
}

void print_bytes_only(char *bytes, int len) {
    int i;
    if (len > 0) {
	for (i=0; i<len; i++) {
	    if( i %32 == 0 ) fprintf( stderr, "\n%04x\t ", i);
	    fprintf( stderr, "%02x ", (int)((unsigned char)bytes[i]));
	    //if( (i+1) % 16 == 0 ) fprintf( stderr, "\n");
	}
    }
}

void trace() {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames 
  backtrace_symbols_fd(array, size, 1);
  exit(1);
}


const int BYTES_PER_LINE=1448;
int lineleft  = 1448;//BYTES_PER_LINE;
int vbi = 0;

int buffer_pos = 0;
int buffer_size;
int next_boundary = 0;
char *pbuffer=NULL;

void check_fence(){
  if( buffer_pos >= buffer_size) return; 
  if( buffer_pos % 0x400 == 0 ) { //on 1024 byte boundaries
    //fprintf( stderr, "boundary at %04x ", buffer_pos );
    next_boundary = buffer_pos + 0x400;
    if( pbuffer[buffer_pos] == (char)0xaa ){ //if the byte is 0xaa
      buffer_pos+=4;	//skip this marker
      //fprintf( stderr, " is video\n");
    }
    else{	//otherwise
      //fprintf( stderr , "\ndiscard block. bytes:");
      //print_bytes_only( pbuffer + buffer_pos + 0x3f0, 32 );
      //fprintf( stderr, "\n");
      buffer_pos += 0x400; //skip the whole 1024 byte block
      next_boundary = buffer_pos + 0x400;
      check_fence();
      //buffer_pos+=4;
      //fprintf( stderr, " is discard\n");
      //Need to check for successive skipped blocks?
    }
  }
}

void init_buffer( char *buffer, int size )
{
  //fprintf( stderr, "buffer init, size %d (%04x)\n", size, size );
  pbuffer = buffer;
  buffer_pos = 0;
  next_boundary = buffer_pos + 0x400;
  buffer_size = size;
  check_fence();
}


int get_buffer_char(){
  if( buffer_pos >= buffer_size) return -1; 
  unsigned char c = pbuffer[buffer_pos];
  buffer_pos++;
  check_fence();
  return c;
}

int write_buffer(int count, unsigned char *frame, int line, int field){
  int written = 0;
  while( buffer_pos < buffer_size && count > 0 )
  {
    int dowrite = MIN( next_boundary - buffer_pos , count );
    //fprintf(stderr, "WROTE %d(%04x)  ",dowrite, dowrite);
    //int wrote = write(2, pbuffer + buffer_pos, dowrite );
    int line_pos = line * (720*2) * 2 + (field * 720 * 2) +  ((720*2) - count ); 
    //fprintf( stderr, "write: line=%d, field=%d, count=%d, off=%d\n", line,field,count,line_pos);
    if( line < 288 ) memcpy(line_pos + frame, pbuffer + buffer_pos, dowrite);
    int wrote = dowrite;
    buffer_pos += wrote;
    count -= wrote;
    written += wrote;
    check_fence();
  }
  return written;
}

int skip_buffer(int count){
  int skipped = 0;
  while( buffer_pos < buffer_size && count > 0 )
  {
    int skip = MIN( next_boundary - buffer_pos , count );
    buffer_pos += skip;
    count -= skip;
    skipped += skip;
    check_fence();
  }
  return skipped;
}

enum sync_state{
  HSYNC,
  SYNCFF,
  SYNCZ1,
  SYNCZ2,
  SYNCAV,
  VBLANK,
  VACTIVE,
  REMAINDER
};

enum sync_state state = HSYNC;
int line_remaining = 0;
int active_line_count = 0;
int vblank_found = 0;
int field = 0;

unsigned char frame[ 720 * 2 * 288 * 2 ];

void process_data() { //buffer_init() has been called
  int next = 0;
  int bs =0;
  int hs=0;
  if( ! (state >= VBLANK )) {
    next = get_buffer_char();
  }
  while( next != -1 ){
    unsigned char nc = next;
    //fprintf( stderr, "%d", state);
    switch( state ){
      case HSYNC:
	hs++;
	if( nc == (unsigned char)0xff ){
	  state = SYNCZ1;
	  if(bs==1)fprintf( stderr, "resync after %d @%d(%04x)\n", hs, buffer_pos, buffer_pos);
	  bs=0;
	}
	else if( bs != 1) {
	  fprintf( stderr, "bad sync on line %d @%d (%04x)\n", active_line_count, buffer_pos, buffer_pos);
	  //print_bytes_only( pbuffer, buffer_pos + 64);
	  //print_bytes( pbuffer + buffer_pos , 8 );
	  bs=1;
	}
	break;
      case SYNCZ1:
	if( nc == (unsigned char)0x00 ) state = SYNCZ2;
	else state = HSYNC;
	break;
      case SYNCZ2:
	if( nc == (unsigned char)0x00 ){
	  state = SYNCAV;
	}
	else{
	  state = HSYNC;
	}
	break;
      case SYNCAV:
	//fprintf(stderr,"%02x",nc);
	if( nc & (unsigned char)0x10 ){ //EAV
	  state = HSYNC;
	}
	else{ //SAV
	  field = ( nc & (char)0x40) ? 1 : 0;
	  if( nc & (unsigned char)0x20 ){ //line is VBlank
	    state = VBLANK;
	    vblank_found++;
	    if(active_line_count > 280 ){
	      if(field==0){
		write( 1 , frame, sizeof(frame) );
		
	      }
	      vblank_found = 0;
	    
	      //fprintf( stderr, "lines: %d\n", active_line_count);
	    }
	    active_line_count = 0;
	  }
	  else{
	    state = VACTIVE; //line is active
	  }
	  line_remaining = 720 * 2;
	}
	break;
      case VBLANK:
      case VACTIVE:
      case REMAINDER:
	if( state == VBLANK || vblank_found < 20 ){//  || state == REMAINDER){
	  line_remaining -= skip_buffer( line_remaining );
	  //fprintf( stderr, "vblank_found=%d\n" , vblank_found);
	}
	else{
	  line_remaining -= write_buffer( line_remaining, frame, active_line_count, field );
	  if( line_remaining <= 0 ) active_line_count ++;

	}
	//fprintf( stderr, "vblank_found: %d, line remaining: %d, line_count: %d\n", vblank_found, line_remaining, active_line_count );
	if( line_remaining <= 0 ){
	  //if( state == REMAINDER ) skip_buffer( 3 );
	  state = HSYNC;
	  
	}
	else{
	  //fprintf( stderr, "\nOn line %d, line_remaining: %d(%04x). bp=%04x/%04x\n", active_line_count, line_remaining, line_remaining , buffer_pos, buffer_size );
	  state=REMAINDER;
	  next=-1; //no more data in this buffer. exit loop
	}
	break;
      
    }
    if( ! (state >= VBLANK  ) ){
      next = get_buffer_char();
    }
  }
}
/*
int sync = 0;
int sync_seek = 1;
int line_count = 0;
int is_vblank = -1;

void find_sync(char* bytes, int len) 
{
  int i = 0;
  fprintf( stderr, "find_sync sync=%d\n", sync );
  while( i <   len )
  {
    if(  bytes[i] == (char)(0xaa) )
    {
      if( sync == 0 ) sync = i;
      //fprintf( stderr, "aa aa 00 00 block i=%d, sync=%d \n", i, sync );
      while( sync < i + 0x400 )
      {
	if( bytes[sync] == (char)0xff && bytes[sync+1] == (char)0x00 && bytes[sync+2] == (char)0x00 )
	{
	    unsigned char seav = bytes[sync+3];
	    //fprintf( stderr, " %02x " , seav );
	    if( seav & (char)0x40){
	      //fprintf( stderr, "@ sync=%d F1 ",sync);
	    }
	    else{
	      //fprintf( stderr, "@ sync=%d F0 ",sync);
	    }
	    if( seav & (char)0x20){
	      //fprintf( stderr, "V1 ");
	      if( is_vblank==0) fprintf( stderr, "V1 lc=%d\n",line_count);
	      is_vblank = 1;
	    }
	    else{
	      if( is_vblank == 0 ) line_count++;
	      
	      if( is_vblank == 1 ){
		line_count=0;
		is_vblank = 0;
	      }
	      //fprintf( stderr, "V0 ");
	    }
	    if( seav & (char)0x10){
	      //fprintf( stderr, "EAV \n");
	      sync+=4;
	    }
	    else{
	      sync_seek = 0;
	      fprintf( stderr, "SAV b=%d", sync % 0x400 );
	      if( sync % 0x400 >= (0x400 -  (1448-0x400) ) ) sync+= 4; //frame adjustment
	      sync+= 720*2 + 4 + 4; //point to next SAV; skip 720*4 pixels + 4 SAV + 4 EAV
	      sync+= 4; // aa aa 00 00 frame marker adjustments
	      fprintf( stderr, "  -> next @ sync=%d \n", sync);
	      
	    }
	    
	      
	}
	else
	{
	  sync++;
	  if( sync_seek == 0 ){
	    fprintf( stderr, "sync miss @expected=%d. @pos=%d: ", sync-1, sync-7);
	    print_bytes(bytes+sync-7,6);
	    print_bytes(bytes+sync-1,6);
	    fprintf( stderr, "\n");
	  }
	  sync_seek = 1;
	}
      }
    }
    else
    {
      sync+= 0x400; //adjust over filler block
      fprintf( stderr, "%02x %02x %02x %02x block i=%d sync(adj)=%d\n",bytes[i],bytes[i+1],bytes[i+2],bytes[i+3],i,sync);
      
    }
    i+= 0x400;
  }
  sync = sync % i;

}




void save_bytes(char *bytes, int len) {
 //if( len >  0 ) write( 2, bytes, len );
 //return;
 int i = 0;
  int j = 0;
  int t = 0;
  //if(vbi == 0 ) i+= 0xc00;
  int line_number = 0;
  while( i < len )
  {
    if(  bytes[i] == (char)(0xaa) )
    {
      j=4;
      
      while( j < 0x400 )
      {
	if( bytes[i+j] == (char)0xff )
	{
	  //fprintf( stderr, "%d: ",line_number);
	  //print_bytes(bytes+i+j,4);
	  //fprintf( stderr, "\n");
	}
	if( (bytes[i+j] == (char)0xff && (bytes[i+j+3] & (unsigned char)0xb0) == (unsigned char)0x80 )  ||  lineleft < BYTES_PER_LINE )
	{
	  
	  int dowrite = MIN( 0x400 - j , lineleft );
	  
	  
	  if( vbi < 20 ) //seek vbi, bit 5 in SAV
	  {
	    if( ( bytes[i+j+3] & (unsigned char)0x20) == (unsigned char)0x20  ) {
	      vbi++; 
	      //fprintf( stderr, "vbi:%d\n",vbi);
	    }
	    //else {
	    //  vbi--; 
	    //  fprintf( stderr, "vbi:%d\n",vbi);
	    //}
	  }

	  if( vbi >= 20 )
	  {
	    lineleft -= dowrite;
	    write( 1, bytes+i+j, dowrite);
	  }
	  j += dowrite;

	  if( lineleft <= 0  )
	  {
	    lineleft  = BYTES_PER_LINE;
	    line_number++;
	  }
	  
	}
	else j++;
      }
    }
    i+= 0x400;
  }
}
*/
char isobuf[64 * 3072];
char isobuf1[64 * 3072];
char isobuf2[64 * 3072];
char isobuf3[64 * 3072];
int pcount= 0;
const int FCOUNT = 800000;
void gotdata( struct libusb_transfer *tfr )
{
  //fprintf( stderr, "id %d:", pcount);
  int num = tfr->num_iso_packets;
  char *data = libusb_get_iso_packet_buffer_simple(tfr, 0 );
  int length = tfr->iso_packet_desc[0].length;
  
  //save_bytes(data, num * length);
  int total=0;
  int i=0;
  for( ;i < num; i++ )
  {
    total+= tfr->iso_packet_desc[i].actual_length;
  }
  
  //fprintf( stderr, "id %d got %d pkts of length %d. calc=%d, total=%d (%04x)\n", pcount, num, length, num*length, total, total );

  pcount++;

  
  if( pcount >= 0 ){
    //find_sync( data, num * length );
    init_buffer( data, num * length );
    //init_buffer( data, total );
    process_data();
    //fprintf( stderr, "write\n"); 
    //write(1, data, total);
    //write(1,"----------------",16);
  }

  if( pcount <= FCOUNT-4 )
  {
    //fprintf( stderr, "resubmit id %d\n", pcount-1 );
    int ret = libusb_submit_transfer( tfr );
    if (ret != 0) {
	fprintf( stderr, "libusb_submit_transfer failed with error %d\n", ret);
	exit(1);
    }
  }
  
}


int main(int argc, char **argv) {
    int ret, vendor, product;
    struct libusb_device *dev;
    char buf[65535], *endptr;

    /*
    usb_urb *isourb;
    struct timeval isotv;
    char isobuf[393216];

    usb_urb *isourb1;
    struct timeval isotv1;
    char isobuf1[393216];
    
    usb_urb *isourb2;
    struct timeval isotv2;
    char isobuf2[393216];

    usb_urb *isourb3;
    struct timeval isotv3;
    char isobuf3[393216];
*/
    libusb_init(NULL);
    libusb_set_debug(NULL,0);

    if (argc!=3) {
	fprintf( stderr, "usage: %s vendorID productID\n", argv[0]);
	exit(1);
    }
    vendor = strtol(argv[1], &endptr, 16);
    if (*endptr != '\0') {
	fprintf( stderr, "invalid vendor id\n");
	exit(1);
    }
    product = strtol(argv[2], &endptr, 16);
    if (*endptr != '\0') {
	fprintf( stderr, "invalid product id\n");
	exit(1);
    }
    dev = find_device(vendor, product);
    assert(dev);

    ret = libusb_open(dev, &devh);
    libusb_unref_device( dev );
    
    assert(ret == 0);
    
    
    
    signal(SIGTERM, release_usb_device);
/*
    ret = usb_get_driver_np(devh, 0, buf, sizeof(buf));
    fprintf( stderr, "usb_get_driver_np returned %d\n", ret);
    if (ret == 0) {
	fprintf( stderr, "interface 0 already claimed by driver \"%s\", attempting to detach it\n", buf);
	ret = usb_detach_kernel_driver_np(devh, 0);
	fprintf( stderr, "usb_detach_kernel_driver_np returned %d\n", ret);
    }
*/    
    ret = libusb_claim_interface(devh, 0);
    if (ret != 0) {
	fprintf( stderr, "claim failed with error %d\n", ret);
		exit(1);
    }
    
    ret = libusb_set_interface_alt_setting(devh, 0, 0);
    if (ret != 0) {
	fprintf( stderr, "set_interface_alt_setting failed with error %d\n", ret);
		exit(1);
    }

ret = libusb_get_descriptor(devh, 0x0000001, 0x0000000, buf, 0x0000012);
fprintf( stderr, "1 get descriptor returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000009);
fprintf( stderr, "2 get descriptor returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000042);
fprintf( stderr, "3 get descriptor returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");

ret = libusb_release_interface(devh, 0);
if (ret != 0) fprintf( stderr, "failed to release interface before set_configuration: %d\n", ret);
ret = libusb_set_configuration(devh, 0x0000001);
fprintf( stderr, "4 set configuration returned %d\n", ret);
ret = libusb_claim_interface(devh, 0);
if (ret != 0) fprintf( stderr, "claim after set_configuration failed with error %d\n", ret);
ret = libusb_set_interface_alt_setting(devh, 0, 0);
fprintf( stderr, "4 set alternate setting returned %d\n", ret);
//usleep(1*1000);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x0000001, 0x0000000, buf, 0x0000002, 1000);
fprintf( stderr, "5 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\xf0\xb3\x79\x03\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "6 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\xf0\xb3\x79\x03\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "7 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\xf0\xb3\x79\x03\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "8 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\xf0\xb3\x79\x03\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "9 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(2500*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\xe1\x03\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "10 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "11 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "12 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "13 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x88\xca\xd7\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "14 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x88\xca\xd7\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "15 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(100*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x88\xca\xd7\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "16 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x88\xca\xd7\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "17 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(12*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "18 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "19 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "20 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "21 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "22 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(55*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "23 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "24 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "25 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "26 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "27 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "28 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(100*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "29 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "30 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "31 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "32 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "33 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "34 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "35 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(2500*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "36 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "37 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "38 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "39 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\x6e\x80\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "40 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x98\x6e\x80\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "41 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(100*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\x6e\x80\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "42 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x98\x6e\x80\x0d\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "43 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "44 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "45 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "46 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "47 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "48 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(42*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "49 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "50 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "51 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "52 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x68\xa3\xe5\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "53 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x68\xa3\xe5\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "54 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(103*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x68\xa3\xe5\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "55 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x68\xa3\xe5\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "56 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "57 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "58 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "59 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "60 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "61 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(25*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "62 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "63 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "64 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "65 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(13*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x68\x13\x2c\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "66 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x68\x13\x2c\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "67 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(100*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x68\x13\x2c\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "68 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x68\x13\x2c\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "69 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "70 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "71 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "72 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "73 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "74 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(82*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "75 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "76 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "77 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "78 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xae\x5c\x07\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "79 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x98\xae\x5c\x07\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "80 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(99*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xae\x5c\x07\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "81 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x98\xae\x5c\x07\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "82 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(30*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "83 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "84 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "85 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x01\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "86 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x00\x89\x75\x32\x2f\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "87 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(25*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "88 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "89 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x34\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "90 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x35\x11\x50\xb3\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "91 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "92 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "93 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(100*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x80\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "94 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3b\x00\x98\xfe\x1f\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "95 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(113*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x01\x08\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "96 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x02\xc7\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "97 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x03\x33\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "98 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x04\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "99 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x05\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "100 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x06\xe9\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "101 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x07\x0d\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "102 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x08\x98\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "103 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x09\x01\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "104 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0a\x80\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "105 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0b\x40\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "106 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0c\x40\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "107 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0d\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "108 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "109 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "110 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "111 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "112 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "113 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "114 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "115 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "116 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "117 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x81\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "118 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(9*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0f\x2a\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "119 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x10\x40\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "120 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x11\x0c\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "121 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x12\x01\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "122 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x13\x80\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "123 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x14\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "124 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x15\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "125 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x16\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "126 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x17\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "127 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x40\x02\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "128 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x58\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "129 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x59\x54\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "130 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x5a\x07\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "131 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x5b\x03\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "132 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x5e\x00\xff\x06\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "133 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x02\xc0\xff\xd0\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "134 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0a\x80\xff\xd0\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "135 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0b\x40\xff\xd0\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "136 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0d\x00\xff\xd0\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "137 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0c\x40\xff\xd0\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "138 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x09\x01\x00\x00\x00\x5a\x12\xa0", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "139 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x02\xc0\xff\x0d\x61\xfc\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "140 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0e\x01\xff\x0d\x61\xfc\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "141 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\x84\x00\x01\x40\x00\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "142 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(18*1000);
memcpy(buf, "\x0b\x4a\xa0\x00\x01\x60\xff\xff\x45\x41\x0e\x78\xce", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "143 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "144 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(20*1000);
memcpy(buf, "\x0b\x4a\x84\x00\x01\x5b\x00\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "145 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xa0\x00\x01\x00\xff\xff\xaa\x0d\x14\x78\xce", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "146 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "147 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(21*1000);
memcpy(buf, "\x0b\x4a\x84\x00\x01\x10\x00\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "148 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(10*1000);
memcpy(buf, "\x0b\x4a\xa0\x00\x01\x00\xff\xff\xa6\xc8\x18\x78\xce", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "149 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "150 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(11*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x5a\x07\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "151 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x59\x54\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "152 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x5b\x83\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "153 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x10\x48\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "154 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(2*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x41\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "155 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x42\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "156 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x43\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "157 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x44\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "158 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x45\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "159 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x46\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "160 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x47\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "161 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x48\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "162 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x49\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "163 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4a\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "164 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4b\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "165 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4c\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "166 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4d\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "167 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4e\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "168 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x4f\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "169 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x50\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "170 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x51\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "171 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x52\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "172 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x53\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "173 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x54\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "174 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x55\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "175 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");

memcpy(buf, "\x0b\x4a\xc0\x01\x01\x56\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "175a control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");


memcpy(buf, "\x0b\x4a\xc0\x01\x01\x57\xff\x00\x20\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "175b control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");






memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0a\x80\x00\x00\x40\x89\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "176 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0b\x40\x00\x00\x40\x89\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "177 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0d\x00\x00\x00\x40\x89\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "178 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x0c\x40\x00\x00\x40\x89\x0e\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "179 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x4a\xc0\x01\x01\x09\x01\x00\x00\x00\xd6\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "180 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(2500*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "181 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "182 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(2*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "183 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(26*1000);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x1b\x00\x00\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "184 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "185 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xbd\xbf\xdf\x02\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "186 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
usleep(250*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\x10\x10\x97\x09\x80", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "187 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x10\x18\x85\xe8\x0d\x80", 0x000000d);
memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\x10\xb8\x08\xed\x0d\x80", 0x000000d);

ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "188 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "189 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
memcpy(buf, "\x01\x05", 0x0000002);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x0000001, 0x0000000, buf, 0x0000002, 1000);
fprintf( stderr, "190 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");

ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000109);
fprintf( stderr, "191 get descriptor returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
//usleep(1*1000);
ret = libusb_set_interface_alt_setting(devh, 0, 2);
//usb_set_altinterface(devh, 2);
fprintf( stderr, "192 set alternate setting returned %d\n", ret);
//memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x1d\x4d\x02\x00\x00\x00", 0x000000d);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xe0\x00\x69\x00\x6e", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "193 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
usleep(30*1000);
/*
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\xe0\x01\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "194 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
usleep(100*1000);
*/

struct libusb_transfer *tfr;
tfr = libusb_alloc_transfer( 64 );
assert( tfr != NULL );

libusb_fill_iso_transfer( tfr, devh, 0x00000082, isobuf, 64*3072, 64 , gotdata, NULL, 2000 );
libusb_set_iso_packet_lengths( tfr, 3072 );


struct libusb_transfer *tfr1;
tfr1 = libusb_alloc_transfer( 64 );
assert( tfr1 != NULL );

libusb_fill_iso_transfer( tfr1, devh, 0x00000082, isobuf1, 64*3072, 64 , gotdata, NULL, 2000 );
libusb_set_iso_packet_lengths( tfr1, 3072 );

struct libusb_transfer *tfr2;
tfr2 = libusb_alloc_transfer( 64 );
assert( tfr2 != NULL );

libusb_fill_iso_transfer( tfr2, devh, 0x00000082, isobuf2, 64*3072, 64 , gotdata, NULL, 2000 );
libusb_set_iso_packet_lengths( tfr2, 3072 );

struct libusb_transfer *tfr3;
tfr3 = libusb_alloc_transfer( 64 );
assert( tfr3 != NULL );

libusb_fill_iso_transfer( tfr3, devh, 0x00000082, isobuf3, 64*3072, 64 , gotdata, NULL, 2000 );
libusb_set_iso_packet_lengths( tfr3, 3072 );



ret = libusb_submit_transfer( tfr );
if (ret != 0) {
	fprintf( stderr, "libusb_submit_transfer failed with error %d\n", ret);
		exit(1);
    }

ret = libusb_submit_transfer( tfr1 );
if (ret != 0) {
	fprintf( stderr, "libusb_submit_transfer failed with error %d\n", ret);
		exit(1);
    }

ret = libusb_submit_transfer( tfr2 );
if (ret != 0) {
	fprintf( stderr, "libusb_submit_transfer failed with error %d\n", ret);
		exit(1);
    }

ret = libusb_submit_transfer( tfr3 );
if (ret != 0) {
	fprintf( stderr, "libusb_submit_transfer failed with error %d\n", ret);
		exit(1);
    }


memcpy(buf, "\x0b\x00\x00\x82\x01\x18\x00\x0d\x00\x01\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
fprintf( stderr, "242 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
fprintf( stderr, "\n");
/*
usleep(1*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x1d\x00\x01\x00\x00\x00", 0x000000d);
ret = libusb_control_transfer(devh,LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
printf("243 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
printf("\n");
usleep(14*1000);
memcpy(buf, "\x0b\x00\x00\x82\x01\x17\x40\x00\x5c\x00\x69\x00\x6e", 0x000000d);
ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 0x000000d, 1000);
printf("244 control msg returned %d, bytes: ", ret);
print_bytes(buf, ret);
printf("\n");
*/


while( pcount < FCOUNT ){
  libusb_handle_events(NULL);
}


//fprintf( stderr, "writing frame...\n");
//write( 1, frame , sizeof(frame) );


libusb_free_transfer( tfr );
libusb_free_transfer( tfr1 );
libusb_free_transfer( tfr2 );
libusb_free_transfer( tfr3 );

/*
ret = usb_isochronous_setup(&isourb, 0x00000082, 3072, isobuf, 64 * 3072);
ret = usb_isochronous_setup(&isourb1, 0x00000082, 3072, isobuf1, 64 * 3072);
ret = usb_isochronous_setup(&isourb2, 0x00000082, 3072, isobuf2, 64 * 3072);
ret = usb_isochronous_setup(&isourb3, 0x00000082, 3072, isobuf3, 64 * 3072);

//fprintf( stderr, "195 isochronous setup returned %d\n", ret);
ret = usb_isochronous_submit(devh, isourb, &isotv);
//fprintf( stderr, "195 isochronous submit returned %d\n", ret);


//fprintf( stderr, "195a isochronous setup returned %d\n", ret);
ret = usb_isochronous_submit(devh, isourb1, &isotv1);
//fprintf( stderr, "195a isochronous submit returned %d\n", ret);

//fprintf( stderr, "195b isochronous setup returned %d\n", ret);
ret = usb_isochronous_submit(devh, isourb2, &isotv2);
//fprintf( stderr, "195b isochronous submit returned %d\n", ret);

//fprintf( stderr, "195c isochronous setup returned %d\n", ret);
ret = usb_isochronous_submit(devh, isourb3, &isotv3);
//fprintf( stderr, "195c isochronous submit returned %d\n", ret);

int r1,r2,r3,r4;
ret = usb_isochronous_reap(devh, isourb, &isotv, 1000);
//fprintf( stderr, "195 isochronous reap returned %d, bytes: ", ret);
r1=ret;

ret = usb_isochronous_reap(devh, isourb1, &isotv1, 1000);
//fprintf( stderr, "195a isochronous reap returned %d, bytes: ", ret);
r2=ret;

ret = usb_isochronous_reap(devh, isourb2, &isotv2, 1000);
//fprintf( stderr, "195b isochronous reap returned %d, bytes: ", ret);
r3=ret;

ret = usb_isochronous_reap(devh, isourb3, &isotv3, 1000);
//fprintf( stderr, "195c isochronous reap returned %d, bytes: ", ret);
r4=ret;

save_bytes(isourb->buffer, r1);
save_bytes(isourb1->buffer, r2);
save_bytes(isourb2->buffer, r3);
save_bytes(isourb3->buffer, r4);

*/
	ret = libusb_release_interface(devh, 0);
	assert(ret == 0);
	libusb_close(devh);
	libusb_exit(NULL);
	return 0;
}
