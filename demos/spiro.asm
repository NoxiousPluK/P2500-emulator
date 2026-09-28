; ---------------------------------------------------------------------------
; SPIRO.COM - Lissajous figures, drawn a point at a time.
;
;   x = 256 + AMPX * sin(ax) / 128      ax += px each step
;   y = 128 + AMPY * sin(ay) / 128      ay += py
;
; Stepping two phase accumulators avoids multiplying by t: the only
; multiplies left are amplitude by sine, which mul_signed does in eight
; shift-and-adds. A hundred points per field keeps inside the frame budget
; and lets the curve draw itself visibly.
;
; Any key returns to CP/M.
; ---------------------------------------------------------------------------

org $0100

AMPX    equ 232
AMPY    equ 116
PERPASS equ 90               ; points per video field
PASSLEN equ 45               ; fields before the figure changes

start:
  call gfx_on
  call vid_in
  call cls
  call vid_out

new_figure:
  xor a
  ld (ax),a
  ld (ay),a
  ld a,PASSLEN
  ld (passes),a
  call vid_in
  call cls
  call vid_out

frame:
  call vid_in
  ld a,PERPASS
  ld (pcount),a
point_loop:
  call one_point
  ld a,(pcount)
  dec a
  ld (pcount),a
  jr nz,point_loop
  call vid_out

  call wait_frame
  call kbhit
  jr nz,done

  ld a,(passes)
  dec a
  ld (passes),a
  jr nz,frame
  call next_params
  jr new_figure

done:
  call vid_in
  call cls
  call vid_out
  call gfx_off
  ret

; --- one plotted point ------------------------------------------------------

one_point:
  ; x = 256 + AMPX * sin(ax) / 128
  ld a,(ax)
  call sine
  ld c,AMPX
  call mul_signed
  call div128
  ld de,256
  add hl,de
  push hl

  ; y = 128 + AMPY * sin(ay) / 128
  ld a,(ay)
  call sine
  ld c,AMPY
  call mul_signed
  call div128
  ld de,128
  add hl,de
  ld a,l
  pop bc
  push af
  call plot
  pop af

  ; advance both phases
  ld a,(ax)
  ld b,a
  ld a,(px)
  add a,b
  ld (ax),a
  ld a,(ay)
  ld b,a
  ld a,(py)
  add a,b
  ld (ay),a
  ret

; a = phase 0-255 -> a = signed sine
sine:
  ld hl,sintab
  ld e,a
  ld d,0
  add hl,de
  ld a,(hl)
  ret

; hl = hl / 128, signed. Seven shifts is more than a rotate-and-swap, but it
; is also obviously right, and this is not the inner loop.
div128:
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
  sra h
  rr l
  sra h
  rr l
  ret

; Walk a short list of frequency pairs, so the figure changes but always
; closes on itself.
next_params:
  ld a,(pidx)
  inc a
  and 7
  ld (pidx),a
  add a,a
  ld e,a
  ld d,0
  ld hl,ptable
  add hl,de
  ld a,(hl)
  ld (px),a
  inc hl
  ld a,(hl)
  ld (py),a
  ret

include "p2500.inc"

ax:      db 0
ay:      db 0
px:      db 3
py:      db 5
pidx:    db 0
pcount:  db 0
passes:  db 0

; (x step, y step). Coprime pairs close the figure; keeping the steps small
; keeps the dots close enough together to read as a curve, since there is no
; line drawing here - each point stands on its own.
ptable:
  db 3,5
  db 2,3
  db 1,2
  db 3,4
  db 4,5
  db 1,3
  db 5,6
  db 2,5

sintab:
  db 0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46
  db 49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88
  db 90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116
  db 117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127
  db 127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118
  db 117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92
  db 90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51
  db 49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3
  db 0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46
  db -49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88
  db -90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116
  db -117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127
  db -127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118
  db -117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92
  db -90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51
  db -49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3