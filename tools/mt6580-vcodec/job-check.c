// SPDX-License-Identifier: GPL-2.0-only
/* Check nonblocking ownership semantics without programming a DMA command. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../include/uapi/linux/mt6580-vcodec.h"
int main(void)
{
    struct mt6580_vcodec_buffer buffer = {.fd=-1,.size=4096};
    struct mt6580_vcodec_oal oal = {0};
    struct mt6580_vcodec_job job = {.driver_type=6,.timeout_ms=1000,.handle=0x504f4c4c};
    int fd=open("/dev/mt6580-vcodec",O_RDWR|O_CLOEXEC), result=1;
    uint32_t *header=MAP_FAILED;
    if(fd<0 || ioctl(fd,MT6580_VCODEC_ALLOC,&buffer))goto done;
    header=mmap64(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,buffer.mmap_offset);
    if(header==MAP_FAILED)goto done;
    memset(header,0,4096);oal.header_id=buffer.id;
    if(ioctl(fd,MT6580_VCODEC_OAL_BIND,&oal)||ioctl(fd,MT6580_VCODEC_JOB_ACQUIRE,&job))goto done;
    job.timeout_ms=0;
    errno=0;
    if(ioctl(fd,MT6580_VCODEC_JOB_WAIT,&job)!=-1||errno!=EAGAIN||header[0]||header[1])goto done;
    /* Deliberately cancel this empty lock. No hardware DMA was submitted. */
    if(ioctl(fd,MT6580_VCODEC_JOB_RELEASE,&job)||header[0]!=1||header[1]!=1)goto done;
    puts("JOB_POLL_PENDING_EAGAIN active_header_unchanged=1 explicit_empty_cancel=1");
    result=0;
done:
    if(result)fprintf(stderr,"job-check failed errno=%d\n",errno);
    if(header!=MAP_FAILED)munmap(header,4096);
    if(buffer.id){struct mt6580_vcodec_buffer release={.id=buffer.id};ioctl(fd,MT6580_VCODEC_RELEASE,&release);}
    if(fd>=0)close(fd);
    return result;
}
