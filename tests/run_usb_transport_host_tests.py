"""Compile production USB routines with deterministic transport faults and sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    while source[source.index(')', start) + 1:].lstrip().startswith(';'):
        start = source.index(signature, start + len(signature))
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_INVALID_SIZE -4
#define ESP_ERR_TIMEOUT -5
#define ESP_ERR_INVALID_RESPONSE -6
#define ESP_ERR_NO_MEM -7
#define ESP_RETURN_ON_FALSE(c,e,...) do { if (!(c)) return (e); } while (0)
#define ESP_RETURN_ON_ERROR(expr,...) do {int r=(expr);if(r) return r;}while(0)
#define ESP_GOTO_ON_FALSE(c,e,l,...) do {if(!(c)){ret=(e);goto l;}}while(0)
#define ESP_GOTO_ON_ERROR(expr,l,...) do {int r=(expr);if(r){ret=r;goto l;}}while(0)
#define ESP_LOGD(...)
#define ESP_LOGE(...)
#define ESP_LOGW(...)
#define ESP_LOGI(...)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef int BaseType_t;
typedef int TickType_t;
static const char *esp_err_to_name(int ret) {return "mock";}
'''

RNDIS_STUBS = r'''
#include "rndis_response.h"
#define USB_BM_REQUEST_TYPE_TYPE_CLASS 0x20
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 1
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
#define USB_BM_REQUEST_TYPE_DIR_IN 0x80
#define CDC_REQ_SEND_ENCAPSULATED_COMMAND 0
#define CDC_REQ_GET_ENCAPSULATED_RESPONSE 1
typedef struct {void *cdc_port;uint32_t request_id,max_transfer_pkts,max_transfer_size;} usbh_rndis_t;
static bool send_fail, short_response, stale_response, overflow_response;
static int send_count, receive_count;
static uint32_t last_type,last_id;
static int usbh_cdc_send_custom_request(void *port,int type,int request,int value,int index,int length,uint8_t *data) {
    send_count++;
    memcpy(&last_type,data,4); memcpy(&last_id,data+8,4);
    for(int i=(last_type==REMOTE_NDIS_QUERY_MSG?28:length);i<length;i++) assert(data[i]==0);
    return send_fail ? ESP_FAIL : ESP_OK;
}
static int usbh_cdc_send_custom_request_ex(void *port,int type,int request,int value,int index,int length,uint8_t *data,size_t *actual) {
    receive_count++; memset(data,0,length);
    uint32_t header[4]={last_type|0x80000000u,16,last_id+(stale_response?1:0),0};
    if(last_type==REMOTE_NDIS_INITIALIZE_MSG) header[1]=sizeof(rndis_initialize_cmplt_t);
    if(last_type==REMOTE_NDIS_QUERY_MSG) {
        header[1]=28;
        uint32_t info[3]={overflow_response?200u:4u,16,0x12345678};
        memcpy(data+16,info,sizeof(info));
    }
    memcpy(data,header,sizeof(header));
    *actual=short_response?12:header[1];
    return ESP_OK;
}
'''

RNDIS_TESTS = r'''
int main(void) {
    uint8_t packet[256]={0},out[32]; uint32_t length=99;
    rndis_query_cmplt_t header={REMOTE_NDIS_QUERY_CMPLT,32,7,RNDIS_STATUS_SUCCESS,4,20};
    memcpy(packet,&header,sizeof(header)); memcpy(packet+28,"test",4);
    assert(rndis_query_copy(packet,32,7,out,sizeof(out),4,&length));
    assert(length==4 && memcmp(out,"test",4)==0);
    for(int mutation=0;mutation<9;mutation++) {
        rndis_query_cmplt_t invalid=header;
        switch(mutation) {
        case 0: invalid.MessageType=REMOTE_NDIS_KEEPALIVE_CMPLT;break;
        case 1: invalid.MessageLength=257;break;
        case 2: invalid.RequestId=8;break;
        case 3: invalid.Status=RNDIS_STATUS_FAILURE;break;
        case 4: invalid.InformationBufferLength=200;break;
        case 5: invalid.InformationBufferLength=3;break;
        case 6: invalid.InformationBufferOffset=0;break;
        case 7: invalid.InformationBufferOffset=UINT32_MAX;break;
        case 8: invalid.MessageLength=31;break;
        }
        memcpy(packet,&invalid,sizeof(invalid)); memset(out,0x5a,sizeof(out)); length=99;
        assert(!rndis_query_copy(packet,32,7,out,sizeof(out),4,&length));
        assert(length==0);for(size_t i=0;i<sizeof(out);i++)assert(out[i]==0x5a);
    }
    assert(!rndis_query_copy(NULL,0,7,out,32,4,&length));
    assert(!rndis_query_copy(packet,0,7,out,32,4,NULL));
    memcpy(packet,&header,sizeof(header));
    assert(!rndis_query_copy(packet,31,7,out,32,4,&length));
    uint32_t seed=42;
    for(int i=0;i<100000;i++) {
        for(size_t j=0;j<sizeof(packet);j++){seed=seed*1664525u+1013904223u;packet[j]=(uint8_t)(seed>>24);}
        if(i%2) {
            rndis_query_cmplt_t fuzz={REMOTE_NDIS_QUERY_CMPLT,24+(seed%233),7,0,
                                      (seed>>8)%257,(seed>>16)%257};
            memcpy(packet,&fuzz,sizeof(fuzz));
        }
        rndis_query_copy(packet,i%257,7,out,32,4,&length);
    }
    usbh_rndis_t dev={0};
    assert(usbh_rndis_query_msg_request(&dev,123,4,out,sizeof(out),&length)==ESP_OK && length==4);
    assert(usbh_rndis_query_msg_request(&dev,123,4,out,3,&length)==ESP_ERR_INVALID_RESPONSE);
    assert(usbh_rndis_query_msg_request(&dev,123,256,out,32,&length)==ESP_ERR_INVALID_SIZE);
    overflow_response=true;
    assert(usbh_rndis_query_msg_request(&dev,123,4,out,32,&length)==ESP_ERR_INVALID_RESPONSE);
    overflow_response=false;
    short_response=true;
    assert(usbh_rndis_query_msg_request(&dev,123,4,out,32,&length)==ESP_ERR_INVALID_RESPONSE);
    assert(usbh_rndis_init_msg_request(&dev)==ESP_ERR_INVALID_RESPONSE);
    short_response=false;stale_response=true;
    assert(usbh_rndis_keepalive(&dev)==ESP_ERR_INVALID_RESPONSE);
    stale_response=false;
    assert(usbh_rndis_init_msg_request(&dev)==ESP_OK);
    assert(usbh_rndis_keepalive(&dev)==ESP_OK);
    assert(usbh_rndis_set_msg_request(&dev,123,out,4)==ESP_OK);
    int reads=receive_count;send_fail=true;
    assert(usbh_rndis_init_msg_request(&dev)==ESP_FAIL);
    assert(usbh_rndis_set_msg_request(&dev,123,out,4)==ESP_FAIL);
    assert(usbh_rndis_keepalive(&dev)==ESP_FAIL);
    assert(receive_count==reads);
    puts("RNDIS_PASS: response bounds, capacity, offset, type/status/request, short replies, send failures, 100000 fuzz inputs");
}
'''

CDC_STUBS = r'''
#define CDC_CTRL_TIMEOUT_MS 5000
#define USB_BM_REQUEST_TYPE_DIR_IN 0x80
#define USB_TRANSFER_STATUS_COMPLETED 0
#define USB_TRANSFER_STATUS_NO_DEVICE 1
#define USB_TRANSFER_STATUS_CANCELED 2
#define USB_B_ENDPOINT_ADDRESS_EP_NUM_MASK 15
typedef struct {int available;} Semaphore;
typedef Semaphore* SemaphoreHandle_t;
static int xSemaphoreTake(Semaphore *s,int wait) {assert(s);if(!s->available)return 0;s->available--;return 1;}
static int xSemaphoreGive(Semaphore *s) {assert(s && s->available==0);s->available++;return 1;}
typedef struct {uint8_t bmRequestType,bRequest;uint16_t wValue,wIndex,wLength;} usb_setup_packet_t;
typedef struct {uint8_t data_buffer[272];size_t data_buffer_size;int num_bytes,actual_num_bytes,status,bEndpointAddress;void*context;} usb_transfer_t;
typedef struct {void *dev_hdl;struct{usb_transfer_t*xfer;Semaphore*mux;}ctrl;} usbh_cdc_dev_t;
typedef struct {usbh_cdc_dev_t*cdc_dev;struct{Semaphore*out_mux,*out_xfer_free_sem;usb_transfer_t*out_xfer;}data;void*out_ringbuf_handle;bool to_close;} usbh_cdc_port_t;
typedef void* usbh_cdc_port_handle_t;
typedef struct {void*cdc_client_hdl;} cdc_obj_t;
static cdc_obj_t installed;static cdc_obj_t*p_usbh_cdc_obj=&installed;
static int submit_error,reply_size,bulk_submissions,ring_pop_error;
static size_t ring_size=4;
static bool is_port_opened(usbh_cdc_port_t*p){return p!=NULL;}
static size_t _get_ringbuf_len(void*p){return ring_size;}
static int _ringbuf_pop(void*p,uint8_t*d,size_t n,size_t*a,int wait){memset(d,0,n);*a=n;return ring_pop_error;}
static int _ringbuf_push(void*p,const uint8_t*d,size_t n,int wait){return ESP_OK;}
static int usb_host_transfer_submit(usb_transfer_t*x){bulk_submissions++;return submit_error;}
static int usb_host_transfer_submit_control(void*c,usb_transfer_t*x){
    if(submit_error)return submit_error;
    memset(x->data_buffer+8,0xa5,264);x->actual_num_bytes=reply_size;x->status=USB_TRANSFER_STATUS_COMPLETED;
    xSemaphoreGive(x->context);return ESP_OK;
}
static int _cdc_reset_transfer_endpoint(void*h,usb_transfer_t*x){return ESP_OK;}
static esp_err_t _cdc_tx_xfer_submit(usb_transfer_t *out_xfer);
'''

CDC_TESTS = r'''
int main(void) {
    Semaphore mux={1},done={0},outmux={1},free_sem={1};
    usb_transfer_t ctrl={.data_buffer_size=264,.context=&done};
    usbh_cdc_dev_t dev={.ctrl={&ctrl,&mux}};
    usb_transfer_t out={.data_buffer_size=16};
    usbh_cdc_port_t port={.cdc_dev=&dev,.data={&outmux,&free_sem,&out}};out.context=&port;
    uint8_t data[256];size_t actual=999;reply_size=12;
    memset(data,0x7f,sizeof(data));
    assert(usbh_cdc_send_custom_request_ex(&port,0x80,1,0,0,256,data,&actual)==ESP_OK);
    assert(actual==4 && data[0]==0xa5 && data[3]==0xa5 && data[4]==0 && data[255]==0 && mux.available==1);
    for(int invalid=0;invalid<2;invalid++) {
        reply_size=invalid?265:7;
        assert(usbh_cdc_send_custom_request_ex(&port,0x80,1,0,0,256,data,&actual)==ESP_ERR_INVALID_RESPONSE);
        assert(actual==0 && mux.available==1);
    }
    ctrl.data_buffer_size=256;
    assert(usbh_cdc_send_custom_request_ex(&port,0x80,1,0,0,256,data,&actual)==ESP_ERR_INVALID_ARG);
    ctrl.data_buffer_size=264;submit_error=ESP_FAIL;
    assert(usbh_cdc_send_custom_request_ex(&port,0x80,1,0,0,256,data,&actual)==ESP_FAIL && mux.available==1);
    assert(usbh_cdc_write_bytes(&port,data,17,10)==ESP_ERR_INVALID_SIZE && outmux.available==1);
    for(int i=0;i<50;i++) {
        assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_FAIL);
        assert(outmux.available==1 && free_sem.available==1);
    }
    submit_error=0;
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_OK && free_sem.available==0);
    out.status=3;out_xfer_cb(&out);assert(free_sem.available==1);
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_OK);
    out.status=USB_TRANSFER_STATUS_CANCELED;out_xfer_cb(&out);assert(free_sem.available==1);
    port.to_close=true;
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_ERR_INVALID_STATE && free_sem.available==1);
    port.to_close=false;port.out_ringbuf_handle=&port;submit_error=ESP_FAIL;
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_FAIL && free_sem.available==1 && outmux.available==1);
    submit_error=ESP_OK;ring_pop_error=ESP_FAIL;
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_FAIL && free_sem.available==1 && outmux.available==1);
    ring_pop_error=ESP_OK;ring_size=0;
    assert(usbh_cdc_write_bytes(&port,data,4,10)==ESP_OK && free_sem.available==1);
    reply_size=8;
    assert(usbh_cdc_send_custom_request_ex(&port,0,1,0,0,0,NULL,&actual)==ESP_OK && actual==0);
    puts("CDC_PASS: setup/payload capacity, short actual lengths, 50 submit failures, mutex/token release, canceled/failed completion, closing, ring-buffer failures, zero-length control");
}
'''

with tempfile.TemporaryDirectory(prefix='vanex-usb-host-') as tmp:
    tmp = Path(tmp)
    flags = ['-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
    rndis = (ROOT/'components/iot_usbh_rndis/iot_usbh_rndis.c').read_text(encoding='utf-8')
    cdc = (ROOT/'components/iot_usbh_cdc/iot_usbh_cdc.c').read_text(encoding='utf-8')
    cases = {
        'rndis': COMMON + RNDIS_STUBS + '\n'.join(function(rndis, 'static esp_err_t '+name+'(') for name in (
            'usbh_rndis_init_msg_request', 'usbh_rndis_query_msg_request',
            'usbh_rndis_set_msg_request', 'usbh_rndis_keepalive')) + RNDIS_TESTS,
        'cdc': COMMON + CDC_STUBS + '\n'.join(function(cdc, signature) for signature in (
            'static inline esp_err_t _cdc_host_transfer_submit(', 'static void out_xfer_cb(',
            'static esp_err_t _cdc_tx_xfer_submit(', 'esp_err_t usbh_cdc_send_custom_request_ex(',
            'esp_err_t usbh_cdc_write_bytes(')) + CDC_TESTS,
    }
    for name, source in cases.items():
        path = tmp/(name+'.c')
        path.write_text(source)
        subprocess.run(['gcc', *flags, '-I'+str(ROOT/'components/iot_usbh_rndis/priv_include'),
                        str(path), '-o', str(tmp/name)], check=True)
        subprocess.run([str(tmp/name)], check=True, env=env)
