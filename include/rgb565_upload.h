#ifndef RGB565_UPLOAD_H
#define RGB565_UPLOAD_H
#include <stdint.h>
#include <stddef.h>
#include <arm_neon.h>
typedef struct {uint8_t r[32],g[64],b[32];} UploadColors;
static inline void upload_rgb565_abgr(const uint16_t *src,uint32_t *dst,size_t n,const UploadColors *colors){
    uint8x16x2_t rt={{vld1q_u8(colors->r),vld1q_u8(colors->r+16)}};
    uint8x16x4_t gt={{vld1q_u8(colors->g),vld1q_u8(colors->g+16),vld1q_u8(colors->g+32),vld1q_u8(colors->g+48)}};
    uint8x16x2_t bt={{vld1q_u8(colors->b),vld1q_u8(colors->b+16)}};
    size_t i=0;
    for(;i+8<=n;i+=8){
        uint16x8_t p=vld1q_u16(src+i);
        uint8x8x4_t c;
        c.val[0]=vqtbl2_u8(rt,vmovn_u16(vshrq_n_u16(p,11)));
        c.val[1]=vqtbl4_u8(gt,vmovn_u16(vandq_u16(vshrq_n_u16(p,5),vdupq_n_u16(63))));
        c.val[2]=vqtbl2_u8(bt,vmovn_u16(vandq_u16(p,vdupq_n_u16(31))));
        c.val[3]=vdup_n_u8(255);vst4_u8((uint8_t *)(dst+i),c);
    }
    for(;i<n;i++){uint16_t p=src[i];dst[i]=0xff000000u|((uint32_t)colors->b[p&31]<<16)|((uint32_t)colors->g[(p>>5)&63]<<8)|colors->r[p>>11];}
}
/* Match SDL's pixel-centred nearest 800x600 -> 640x480 presentation before
   upload, so only the pixels actually displayed need conversion/transfer. */
static inline void upload_rgb565_abgr_640(const uint16_t *src,uint32_t *dst,const UploadColors *colors){
    uint8x16x2_t rt={{vld1q_u8(colors->r),vld1q_u8(colors->r+16)}};
    uint8x16x4_t gt={{vld1q_u8(colors->g),vld1q_u8(colors->g+16),vld1q_u8(colors->g+32),vld1q_u8(colors->g+48)}};
    uint8x16x2_t bt={{vld1q_u8(colors->b),vld1q_u8(colors->b+16)}};
    static const uint8_t indices[16]={0,1,2,3,6,7,8,9,10,11,12,13,16,17,18,19};
    uint8x16_t select=vld1q_u8(indices);
    for(int y=0;y<480;y++){
        const uint16_t *row=src+((2*y+1)*5/8)*800;
        uint32_t *out=dst+y*640;
        for(int block=0;block<79;block++){
            const uint16_t *in=row+block*10;
            uint8x16x2_t pair={{vld1q_u8((const uint8_t *)in),vld1q_u8((const uint8_t *)(in+8))}};
            uint16x8_t p=vreinterpretq_u16_u8(vqtbl2q_u8(pair,select));
            uint8x8x4_t c;
            c.val[0]=vqtbl2_u8(rt,vmovn_u16(vshrq_n_u16(p,11)));
            c.val[1]=vqtbl4_u8(gt,vmovn_u16(vandq_u16(vshrq_n_u16(p,5),vdupq_n_u16(63))));
            c.val[2]=vqtbl2_u8(bt,vmovn_u16(vandq_u16(p,vdupq_n_u16(31))));
            c.val[3]=vdup_n_u8(255);vst4_u8((uint8_t *)(out+block*8),c);
        }
        /* Avoid reading past the final row at its right edge. */
        for(int x=632;x<640;x++){
            uint16_t p=row[(2*x+1)*5/8];
            out[x]=0xff000000u|((uint32_t)colors->b[p&31]<<16)|((uint32_t)colors->g[(p>>5)&63]<<8)|colors->r[p>>11];
        }
    }
}

#endif
