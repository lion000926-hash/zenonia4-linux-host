#pragma once
#include <stdint.h>
#include <string.h>

/* Exact pixel-centred nearest 640->800 mapping: four source pixels become
   a,b,c,c,d. Every fourth source row likewise has one repeated output row. */
static inline void expand_640_to_800_rows(uint16_t *dst,int dst_stride,
                                         const uint16_t *src,int src_stride,
                                         int first_y,int end_y){
    int last_source=-1;
    for(int y=first_y;y<end_y;y++){
        int sy=(2*y+1)*2/5;
        uint16_t *out=dst+y*dst_stride;
        if(sy==last_source){memcpy(out,out-dst_stride,800*2);continue;}
        const uint16_t *in=src+sy*src_stride;
        for(int group=0;group<160;group++){
            out[0]=in[0];out[1]=in[1];out[2]=in[2];out[3]=in[2];out[4]=in[3];
            out+=5;in+=4;
        }
        last_source=sy;
    }
}
