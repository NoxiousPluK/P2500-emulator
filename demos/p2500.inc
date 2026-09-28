; ---------------------------------------------------------------------------
; Shared routines for P2500 high-resolution graphics demos.
;
; The screen is 512 x 256, one bit per pixel, MSB leftmost, out of the 16 KB
; the character codes normally occupy. It is NOT a linear framebuffer: the
; CRTC's raster address is wired to address bits 12-13, so it selects one of
; four 4 KB banks and the memory address indexes within a bank.
;
;     addr = VRAM + (y & 3) * 4096 + (y >> 2) * 64 + x / 8
;
; That is why row_addr looks the way it does, and why next_row adds $1000
; three times and then steps back by 12224. See TODO.md T47 - the layout was
; measured by driving CBIOS's own set-point call, not taken from a manual.
;
; Drawing goes straight into video memory rather than through CBIOS: one
; BDOS call per dot is about four hundred times slower and nothing would
; animate. Port $05 maps the video card's DRAM over $8000-$BFFF while bit 3
; keeps the EPROM out of low memory.
; ---------------------------------------------------------------------------

BDOS      equ $0005
PORT_BANK equ $05
BANK_VID  equ $08          ; video DRAM at $8000, EPROM still out
BANK_MAIN equ $0F          ; main DRAM back
VRAM      equ $8000
SCR_BYTES equ 64           ; 512 pixels / 8
SCR_ROWS  equ 256
ESC       equ 27

; Enter graphics mode: ESC 3. CBIOS reprograms the CRTC to 64x64 cells of
; four scanlines, which is what makes it 512 x 256.
gfx_on:
  ld e,ESC
  call conout
  ld e,'3'
  jp conout

gfx_off:
  ld e,ESC
  call conout
  ld e,'4'
  jp conout

conout:
  push hl
  push de
  push bc
  ld c,2
  call BDOS
  pop bc
  pop de
  pop hl
  ret

; Interrupts off while the video card owns $8000-$BFFF: CBIOS's own ISRs
; live above $CC00 and would not notice, but its console output would write
; characters into the middle of the picture.
vid_in:
  di
  ld a,BANK_VID
  out (PORT_BANK),a
  ret

vid_out:
  ld a,BANK_MAIN
  out (PORT_BANK),a
  ei
  ret

; Clear the whole 16 KB. Video memory must already be banked in.
cls:
  ld hl,VRAM
  ld (hl),0
  ld de,VRAM+1
  ld bc,$3FFF
  ldir
  ret

; Address of a pixel row.
;   in : a = y (0-255), b = x byte (0-63)
;   out: hl = address, c = y & 3 (next_row needs it)
row_addr:
  ld d,a
  and 3
  ld c,a
  srl d
  srl d                    ; d = y >> 2
  ld h,0
  ld l,d
  add hl,hl
  add hl,hl
  add hl,hl
  add hl,hl
  add hl,hl
  add hl,hl                ; hl = (y >> 2) * 64
  ld a,c
  rlca
  rlca
  rlca
  rlca                     ; a = (y & 3) * 16, the high-byte half of * 4096
  add a,h
  ld h,a
  ld a,l
  add a,b
  ld l,a
  ld a,h
  adc a,$80                ; + VRAM, picking up the carry from the byte add
  ld h,a
  ret

; Step one pixel row down. Three rows out of four that is the next 4 KB
; bank; the fourth wraps back and moves on by one cell row - 64 - which is
; -3*4096 + 64 = -12224, or +$D040 in 16 bits.
next_row:
  inc c
  ld a,c
  and 3
  ld c,a
  jr z,nr_wrap
  ld a,h
  add a,$10
  ld h,a
  ret
nr_wrap:
  push de
  ld de,$D040
  add hl,de
  pop de
  ret

; Non-zero if a key is waiting. Call with video banked OUT - it is a BDOS
; call and BDOS is not mapped while the video card owns $8000.
kbhit:
  push hl
  push de
  push bc
  ld c,11
  call BDOS
  pop bc
  pop de
  pop hl
  or a
  ret

; Address and bit mask of one pixel.
;   in : bc = x (0-511), a = y (0-255)
;   out: hl = address, e = bit mask, c = y & 3
pixel_addr:
  push af
  ld a,c
  and 7
  ld e,a
  ld a,$80
  inc e
pa_shift:
  dec e
  jr z,pa_got
  rrca
  jr pa_shift
pa_got:
  ld e,a                   ; e = $80 >> (x & 7); MSB is the leftmost pixel
  srl b
  rr c
  srl b
  rr c
  srl b
  rr c                     ; bc = x >> 3
  ld b,c
  pop af
  jp row_addr

; Set / clear one pixel. Video memory must be banked in.
plot:
  call pixel_addr
  ld a,(hl)
  or e
  ld (hl),a
  ret

unplot:
  call pixel_addr
  ld a,e
  cpl
  and (hl)
  ld (hl),a
  ret

; Unsigned 8 x 8 multiply, shift-and-add.
;   in : b, c   out: hl = b * c
mul8:
  ld hl,0
  ld d,0
  ld e,b
  ld a,c
  ld b,8
mul8_loop:
  rrca
  jr nc,mul8_skip
  add hl,de
mul8_skip:
  sla e
  rl d
  djnz mul8_loop
  ret

; Signed multiply: a (signed) by c (unsigned) into hl.
mul_signed:
  ld b,a
  xor a
  ld (mul_neg),a
  bit 7,b
  jr z,mul_go
  ld a,b
  neg
  ld b,a
  ld a,1
  ld (mul_neg),a
mul_go:
  call mul8
  ld a,(mul_neg)
  or a
  ret z
  ld a,h                   ; negate hl
  cpl
  ld h,a
  ld a,l
  cpl
  ld l,a
  inc hl
  ret

mul_neg: db 0

; Wait for the next 50 Hz video field.
;
; There is no vertical-blank line software can poll - the MC6845's status
; register is not wired anywhere the CPU can see it. But CBIOS keeps a 16-bit
; counter at $F436 that its CTC channel-2 interrupt advances once per field,
; and measuring it against the emulator's own clock gives 50.00 Hz exactly.
; Watching its low byte change is therefore a true field sync, and a better
; one than counting T-states: a demo that takes longer than a field to draw
; simply lands on the next boundary instead of drifting.
;
; Interrupts must be on, so call this after vid_out. The counter lives above
; $C000, outside the video window, so the bank setting does not matter.
;
; $F436 belongs to this CP/M build. A disk built from a different donor may
; put CBIOS elsewhere - hence the bail-out, which degrades to a slow demo
; rather than a hung one if the address is wrong.
TICKS equ $F436

wait_field:
  push af
  push bc
  push de
  push hl
  ld hl,TICKS
  ld b,(hl)
  ld de,8000               ; about three fields, then give up
wf_spin:
  ld a,(hl)
  cp b
  jr nz,wf_done
  dec de
  ld a,d
  or e
  jr nz,wf_spin
wf_done:
  pop hl
  pop de
  pop bc
  pop af
  ret
