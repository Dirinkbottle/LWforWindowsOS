#include "../common/protocol.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct agent { int fd; bool hello, created, framed, presented, destroyed; };
static void fail(const char *text) { fprintf(stderr, "FAIL: stage5-agent: %s\n", text); exit(1); }
static bool send_all(int fd, const uint8_t *p, size_t n) { while(n){ssize_t r=send(fd,p,n,0);if(r<=0)return false;p+=r;n-=(size_t)r;}return true; }
static bool send_message(struct agent *a,uint16_t type,uint64_t seq,const uint8_t *payload,uint32_t n){CwBuffer b;bool ok;cw_buffer_init(&b);ok=cw_message_encode(&b,type,0,seq,payload,n)&&send_all(a->fd,b.data,b.length);cw_buffer_destroy(&b);return ok;}
static bool on_message(void *ctx,const CwHeader *h,const uint8_t *p){struct agent *a=ctx;CwHelloAck ha;CwWindowCreate c;CwWindowFrame f;CwWindowPresent pr;uint8_t ack[16];
	if(h->type==CW_MESSAGE_HELLO_ACK){if(!cw_decode_hello_ack(p,h->payload_length,&ha)||ha.selected_version!=1)fail("bad HELLO_ACK");a->hello=true;return true;}
	if(h->type==CW_MESSAGE_WINDOW_CREATE){if(!cw_decode_window_create(p,h->payload_length,&c)||!a->hello||c.window_id!=1||c.surface_width!=800||c.surface_height!=600)fail("bad CREATE");a->created=true;return true;}
	if(h->type==CW_MESSAGE_WINDOW_FRAME){if(!cw_decode_window_frame(p,h->payload_length,&f)||!a->created||f.width!=800||f.height!=600||f.stride!=3200||f.pixel_format!=CW_PIXEL_FORMAT_BGRA8888||f.pixel_bytes!=1920000)fail("bad FRAME");a->framed=true;return true;}
	if(h->type==CW_MESSAGE_WINDOW_PRESENT){if(!cw_decode_window_present(p,h->payload_length,&pr)||!a->framed||!pr.visible||pr.source_x!=0||pr.source_y!=0||pr.source_w!=800||pr.source_h!=600||pr.destination_x!=80||pr.destination_y!=80||pr.destination_w!=800||pr.destination_h!=600)fail("bad PRESENT");cw_store_u64_le(ack,pr.window_id);cw_store_u64_le(ack+8,pr.presentation_sequence);if(!send_message(a,CW_MESSAGE_WINDOW_PRESENT_ACK,pr.presentation_sequence+100,ack,16))fail("ACK send");a->presented=true;return true;}
	if(h->type==CW_MESSAGE_WINDOW_DESTROY){if(!a->presented||h->payload_length!=8||cw_load_u64_le(p)!=1)fail("bad DESTROY");a->destroyed=true;return true;}return false;}
int main(int argc,char **argv){struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(argc==2?(uint16_t)atoi(argv[1]):44604)};struct agent a={0};uint8_t hello[8]={0},bytes[8192];CwDecoder d;if(inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr)!=1) return 1;a.fd=socket(AF_INET,SOCK_STREAM,0);if(a.fd<0||connect(a.fd,(struct sockaddr*)&addr,sizeof(addr))<0)fail("connect");cw_store_u16_le(hello,1);cw_store_u16_le(hello+2,1);cw_store_u32_le(hello+4,CW_PIXEL_FORMAT_MASK_BGRA8888);if(!send_message(&a,CW_MESSAGE_HELLO,1,hello,8))fail("HELLO send");cw_decoder_init(&d);while(!a.destroyed){ssize_t n=recv(a.fd,bytes,sizeof(bytes),0);if(n<=0||!cw_decoder_feed(&d,bytes,(size_t)n,on_message,&a))fail("receive/decode");}cw_decoder_destroy(&d);close(a.fd);puts("stage5 real-client integration: PASS");return 0;}
