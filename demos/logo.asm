; ---------------------------------------------------------------------------
; LOGO.COM - the P2000 wordmark, bouncing. 512 x 256, one bit per pixel.
;
; Smooth horizontal motion comes from eight pre-shifted copies of the sprite
; (tools/mk_sprite.py); the drawing code picks one by (x & 7) and the byte
; offset by (x >> 3). Erase-then-draw, both inside one banked-in window, so
; the whole frame's worth of writes lands between two video fields.
;
; Any key returns to CP/M.
; ---------------------------------------------------------------------------

org $0100

; How many fields between one-pixel steps. The single-pass blit of 21 x 47
; bytes fits comfortably inside one field, so the loop runs at 50 Hz and
; MOVE_EVERY 2 gives 25 px/s - a logo that drifts across the screen in
; fifteen seconds rather than four. Raise it to slow down further; the
; redraw guard below is what makes that free.
MOVE_EVERY equ 2

; The sprite carries a byte of transparent margin each side and a blank row
; top and bottom (mk_sprite --halo), so posx/posy address the margin's corner
; and the visible logo sits 8 px right and 1 px down of it. Bounds keep the
; whole blit - margin included - inside the 64-byte rows.
MAXX  equ (64 - logo_w) * 8 + 7
MAXY  equ 256 - logo_h

start:
  call gfx_on
  call vid_in
  call cls
  call vid_out

  ; Start somewhere off-centre so the first bounce is not symmetric.
  ld hl,37
  ld (posx),hl
  ld a,23
  ld (posy),a

; One pass, one write per byte. Because the sprite carries its own margin
; and the step is a single pixel, a plain store covers wherever it just was
; - so there is no erase pass, nothing is ever briefly blank, and the work
; halves. Erase-then-draw is what made it flicker: for the length of the
; erase the logo was simply not there.
frame:
  ld a,(movetick)
  dec a
  ld (movetick),a
  jr nz,frame_wait
  ld a,MOVE_EVERY
  ld (movetick),a
  call move

  call split_x             ; posx -> xbyte, shift, sprite pointer
  call vid_in
  call draw
  call vid_out

frame_wait:
  call wait_field
  call kbhit
  jr z,frame

  call vid_in
  call cls
  call vid_out
  call gfx_off
  ret

; --- position ---------------------------------------------------------------

; x += dx, y += dy, bouncing at the edges.
;
; The step has to stay within what the sprite margin covers, because there is
; no erase pass: 8 pixels horizontally, but only ONE row vertically. dy must
; be +/-1 or the logo leaves a trail behind it - which is exactly what a
; leftover -2 in here did.
move:
  ld hl,(posx)
  ld a,(dx)
  ld e,a
  add a,a
  sbc a,a                  ; sign-extend dx into de
  ld d,a
  add hl,de
  bit 7,h
  jr nz,hit_left
  push hl
  ld de,MAXX+1
  or a
  sbc hl,de
  pop hl
  jr nc,hit_right
  ld (posx),hl
  jr move_y
hit_left:
  ld hl,0
  ld (posx),hl
  ld a,1
  ld (dx),a
  jr move_y
hit_right:
  ld hl,MAXX
  ld (posx),hl
  ld a,-1
  ld (dx),a

move_y:
  ld a,(posy)
  ld b,a
  ld a,(dy)
  add a,b
  ; dy is two's complement, so this is a subtract in disguise: carry SET
  ; means no borrow. Underflow is carry CLEAR - the other way round from how
  ; it reads. With an erase pass this bug was invisible, because the clamp to
  ; 0 was drawn over cleanly; without one it leaves the old sprite behind.
  jr nc,hit_top
  cp MAXY+1
  jr nc,hit_bottom
  ld (posy),a
  ret
hit_top:
  xor a
  ld (posy),a
  ld a,1
  ld (dy),a
  ret
hit_bottom:
  ld a,MAXY
  ld (posy),a
  ld a,-1
  ld (dy),a
  ret

; posx -> xbyte, shift, and the pre-shifted copy to draw from.
split_x:
  ld hl,(posx)
  ld a,l
  and 7
  ld (shift),a
  srl h
  rr l
  srl h
  rr l
  srl h
  rr l
  ld a,l
  ld (xbyte),a
  ; sprite = logo + shift * logo_sz
  ld hl,logo
  ld a,(shift)
  or a
  jr z,sx_done
  ld de,logo_sz
sx_add:
  add hl,de
  dec a
  jr nz,sx_add
sx_done:
  ld (sprptr),hl
  ret

; --- blitting ---------------------------------------------------------------

; Store the sprite at (posx, posy). A store, not an OR: the margin bytes are
; blank and have to actually clear what was under them.
draw:
  ld a,(xbyte)
  ld b,a
  ld a,(posy)
  call row_addr
  ld de,(sprptr)
  ld a,logo_h
  ld (rowcnt),a
dr_row:
  push hl
  ld b,logo_w
dr_col:
  ld a,(de)
  ld (hl),a
  inc de
  inc hl
  djnz dr_col
  pop hl
  call next_row
  ld a,(rowcnt)
  dec a
  ld (rowcnt),a
  jr nz,dr_row
  ret

include "p2500.inc"

; --- state ------------------------------------------------------------------

posx:   dw 0
posy:   db 0
dx:     db 1
dy:     db 1
movetick: db 1
xbyte:  db 0
shift:  db 0
sprptr: dw 0
rowcnt: db 0

include "logo_sprite.asm"
