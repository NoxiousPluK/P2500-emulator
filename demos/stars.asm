; ---------------------------------------------------------------------------
; STARS.COM - a starfield, flying forward. 512 x 256, one bit per pixel.
;
; Each star has a fixed direction from the centre and a distance z that grows
; every frame; the screen position is centre + direction * z / 32. Growing z
; linearly makes the apparent speed grow with it, which is what gives the
; sense of moving forward. A star that leaves the screen is restarted close
; in, on the next direction in the table, so the pattern never repeats
; exactly.
;
; Any key returns to CP/M.
; ---------------------------------------------------------------------------

org $0100

NSTARS  equ 48
REC     equ 8                ; bytes per star, a power of two so indexing shifts
CX      equ 256
CY      equ 128
; Distance gained per field, 8.8. 160/256 is about five eighths of a unit,
; which is roughly a third slower than the whole unit it used to be - and
; being fractional it still moves something every field rather than
; stepping at half the frame rate.
ZSTEP   equ 160

; record layout
F_DX    equ 0
F_DY    equ 1
F_Z     equ 2                ; distance, 8.8 fixed point
F_ZH    equ 7
F_OXL   equ 3
F_OXH   equ 4
F_OY    equ 5
F_VIS   equ 6

start:
  call gfx_on
  call vid_in
  call cls
  call vid_out

frame:
  call vid_in
  ld ix,stars
  ld a,NSTARS
  ld (count),a
star_loop:
  call one_star
  ld de,REC
  add ix,de
  ld a,(count)
  dec a
  ld (count),a
  jr nz,star_loop
  call vid_out

  call wait_field
  call kbhit
  jr z,frame

  call vid_in
  call cls
  call vid_out
  call gfx_off
  ret

; --- one star ---------------------------------------------------------------

one_star:
  ; erase where it was, if it was on screen
  ld a,(ix+F_VIS)
  or a
  jr z,os_move
  ld c,(ix+F_OXL)
  ld b,(ix+F_OXH)
  ld a,(ix+F_OY)
  call unplot

os_move:
  ; z += ZSTEP/256, restart when the whole part runs past 255
  ld a,(ix+F_Z)
  add a,ZSTEP
  ld (ix+F_Z),a
  ld a,(ix+F_ZH)
  adc a,0
  ld (ix+F_ZH),a
  jr nz,os_pos
  call restart
  ret

os_pos:
  ; x = CX + dx * z / 32
  ld c,(ix+F_ZH)
  ld a,(ix+F_DX)
  call mul_signed
  call div32
  ld de,CX
  add hl,de
  ; off the left or right edge?
  bit 7,h
  jr nz,os_gone
  ld a,h
  cp 2
  jr nc,os_gone
  push hl                  ; keep x

  ; y = CY + dy * z / 32
  ld c,(ix+F_ZH)
  ld a,(ix+F_DY)
  call mul_signed
  call div32
  ld de,CY
  add hl,de
  ld a,h
  or a
  jr nz,os_gone_x          ; y outside 0..255
  ld a,l
  pop bc                   ; bc = x
  ; remember and draw
  ld (ix+F_OXL),c
  ld (ix+F_OXH),b
  ld (ix+F_OY),a
  ld (ix+F_VIS),1
  jp plot

os_gone_x:
  pop hl
os_gone:
  ld (ix+F_VIS),0
  call restart
  ret

; Restart a star close to the centre, pointing somewhere else.
restart:
  ld a,(ix+F_DX)
  ld b,a
  ld a,(ix+F_DY)
  ld (ix+F_DX),a           ; rotate the pair - cheap, and it never repeats
  ld a,b
  neg
  ld (ix+F_DY),a
  ld a,(seed)
  add a,37
  ld (seed),a
  and 31
  add a,12
  ld (ix+F_ZH),a
  xor a
  ld (ix+F_Z),a
  ld (ix+F_VIS),0
  ret

; hl = hl / 32, signed.
div32:
  sra h
  rr l
  sra h
  rr l
  sra h
  rr l
  sra h
  rr l
  sra h
  rr l
  ret

include "p2500.inc"

count: db 0
seed:  db 91

stars:
  db 34,2,8,0,0,0,0,0,40,6,19,0,0,0,0,0,44,10,30,0,0,0,0,0,48,15,41,0,0,0,0,0
  db 28,12,52,0,0,0,0,0,30,16,63,0,0,0,0,0,30,21,74,0,0,0,0,0,28,27,85,0,0,0,0,0
  db 14,18,96,0,0,0,0,0,11,23,107,0,0,0,0,0,7,27,118,0,0,0,0,0,0,31,129,0,0,0,0,0
  db -5,20,140,0,0,0,0,0,-11,23,151,0,0,0,0,0,-19,26,12,0,0,0,0,0,-29,28,23,0,0,0,0,0
  db -23,17,34,0,0,0,0,0,-32,18,45,0,0,0,0,0,-41,17,56,0,0,0,0,0,-51,16,67,0,0,0,0,0
  db -36,8,78,0,0,0,0,0,-44,7,89,0,0,0,0,0,-53,4,100,0,0,0,0,0,-60,0,111,0,0,0,0,0
  db -40,-3,122,0,0,0,0,0,-45,-7,133,0,0,0,0,0,-50,-11,144,0,0,0,0,0,-29,-9,155,0,0,0,0,0
  db -33,-14,16,0,0,0,0,0,-34,-19,27,0,0,0,0,0,-34,-24,38,0,0,0,0,0,-18,-17,49,0,0,0,0,0
  db -16,-21,60,0,0,0,0,0,-13,-26,71,0,0,0,0,0,-7,-31,82,0,0,0,0,0,0,-20,93,0,0,0,0,0
  db 6,-23,104,0,0,0,0,0,13,-27,115,0,0,0,0,0,22,-29,126,0,0,0,0,0,18,-18,137,0,0,0,0,0
  db 27,-19,148,0,0,0,0,0,36,-20,9,0,0,0,0,0,46,-19,20,0,0,0,0,0,33,-10,31,0,0,0,0,0
  db 42,-9,42,0,0,0,0,0,50,-7,53,0,0,0,0,0,58,-4,64,0,0,0,0,0,39,0,75,0,0,0,0,0