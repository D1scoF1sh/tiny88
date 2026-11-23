; KERNELS.ASM - the 8088 inner loops of TINY88 (Watcom large model, __cdecl)
;
; Assemble: wasm -0 -ml KERNELS.ASM
;
; All routines are FAR, __cdecl: arguments on the stack, caller pops,
; AX BX CX DX ES may be changed, SI DI BP DS are kept.
; Stack after "push bp / mov bp,sp": [bp+6] is the first argument.

.8086
KERNELS_TEXT segment byte public 'CODE'
        assume cs:KERNELS_TEXT

public _mv_u8, _axpy_u8, _vpoke, _vblit

; work variables kept in the code segment (BP and all registers are busy)
v_skip  dw 0
v_xoff  dw 0
v_nblk  dw 0
v_rows  dw 0

; void mv_u8(const unsigned char far *w, unsigned stride, unsigned xoff,
;            unsigned cols, unsigned rows, unsigned long far *out)
;
; out[r] = sum over c < cols of w[r*stride + c] * x[c]      (all unsigned)
; x is in the stack segment (DGROUP) at offset xoff.  cols is a multiple
; of 8, at most 256, and every sum must stay below 2^24 (255*255*256 does).
;
; Per multiply: lodsb / mul byte [bp+d8] / add bx,ax / adc dl,ch
; = 8 instruction bytes, about 105 clocks on a 4.77 MHz 8088.
_mv_u8 proc far
        push    bp
        mov     bp,sp
        push    si
        push    di
        push    ds
        mov     ax,[bp+10]              ; stride
        sub     ax,[bp+14]              ; - cols = bytes to skip per row
        mov     cs:v_skip,ax
        mov     ax,[bp+12]
        mov     cs:v_xoff,ax
        mov     ax,[bp+14]
        mov     cl,3
        shr     ax,cl
        mov     cs:v_nblk,ax            ; blocks of 8 columns (< 256)
        mov     ax,[bp+16]
        mov     cs:v_rows,ax
        les     di,[bp+18]              ; out
        lds     si,[bp+6]               ; weights
        cld
        or      ax,ax
        jz      mv_done
mv_row:
        mov     bp,cs:v_xoff
        xor     bx,bx                   ; DL:BX = 24-bit sum
        xor     dx,dx
        mov     cx,cs:v_nblk            ; CH = 0 (used by adc dl,ch)
mv_blk:
        lodsb
        mul     byte ptr [bp+0]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+1]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+2]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+3]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+4]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+5]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+6]
        add     bx,ax
        adc     dl,ch
        lodsb
        mul     byte ptr [bp+7]
        add     bx,ax
        adc     dl,ch
        add     bp,8
        loop    mv_blk
        mov     ax,bx
        stosw
        mov     ax,dx                   ; DH is 0
        stosw
        add     si,cs:v_skip
        dec     word ptr cs:v_rows
        jnz     mv_row
mv_done:
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret
_mv_u8 endp

; void axpy_u8(unsigned accoff, const unsigned char far *v, unsigned c, unsigned n)
;
; acc[i] += (unsigned long)c * v[i] for i < n.  acc is an array of
; unsigned longs in the stack segment (DGROUP) at offset accoff.
_axpy_u8 proc far
        push    bp
        mov     bp,sp
        push    si
        push    di
        push    ds
        mov     di,[bp+6]
        lds     si,[bp+8]
        mov     bx,[bp+12]
        mov     cx,[bp+14]
        push    ss
        pop     es
        cld
        jcxz    ax_done
ax_lp:
        lodsb
        xor     ah,ah
        mul     bx
        add     es:[di],ax
        adc     es:[di+2],dx
        add     di,4
        loop    ax_lp
ax_done:
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret
_axpy_u8 endp

; void vpoke(unsigned seg, unsigned off, unsigned cell, unsigned snow)
;
; Store one character+attribute word in video memory.  With snow != 0
; (IBM CGA in 80 column text) wait for horizontal retrace first, so the
; screen does not show "snow".
_vpoke proc far
        push    bp
        mov     bp,sp
        mov     es,[bp+6]
        mov     bx,[bp+8]
        mov     cx,[bp+10]
        cmp     word ptr [bp+12],0
        je      vp_store
        mov     dx,3DAh
        cli
vp_w1:  in      al,dx                   ; wait while in retrace
        test    al,1
        jnz     vp_w1
vp_w2:  in      al,dx                   ; wait for retrace to start
        test    al,1
        jz      vp_w2
vp_store:
        mov     es:[bx],cx
        sti
        pop     bp
        ret
_vpoke endp

; void vblit(unsigned seg, unsigned off, const unsigned far *src, unsigned n, unsigned snow)
;
; Copy n words to video memory, with the same snow handling per word.
_vblit proc far
        push    bp
        mov     bp,sp
        push    si
        push    di
        push    ds
        mov     es,[bp+6]
        mov     di,[bp+8]
        lds     si,[bp+10]
        mov     cx,[bp+14]
        cld
        jcxz    vb_done
        cmp     word ptr [bp+16],0
        jne     vb_snow
        rep     movsw
        jmp     short vb_done
vb_snow:
        mov     dx,3DAh
vb_lp:
        lodsw
        mov     bx,ax
        cli
vb_w1:  in      al,dx
        test    al,1
        jnz     vb_w1
vb_w2:  in      al,dx
        test    al,1
        jz      vb_w2
        mov     ax,bx
        stosw
        sti
        loop    vb_lp
vb_done:
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret
_vblit endp

KERNELS_TEXT ends
        end
