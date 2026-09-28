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

; How many fields between one-pixel steps. At 160 x 51 the erase and blit
; take a little over one field, so the field sync settles on two fields per
; redraw and MOVE_EVERY 1 already gives 25 px/s - a logo that drifts across
; the screen in fifteen seconds rather than four. Raise it to slow down
; further; the redraw guard below then earns its keep.
MOVE_EVERY equ 1

MAXX  equ 512 - logo_pix - 1
MAXY  equ 256 - logo_h - 1

start:
  call gfx_on
  call vid_in
  call cls
  call vid_out

  ; Start somewhere off-centre so the first bounce is not symmetric.
  ld hl,37
  ld (posx),hl
  ld (oldx),hl
  ld a,23
  ld (posy),a
  ld (oldy),a

; Redraw only on the fields where the logo actually moved. Erasing and
; blitting 51 rows twice over costs more than a field, so doing it when
; nothing has changed would both waste the time and leave a half-drawn
; sprite on screen for longer than it needs to be. With MOVE_EVERY at 1 it
; moves every pass anyway; the guard is what makes raising it cheap.
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
  call erase               ; old position first - they usually overlap
  call draw
  call vid_out

  ld hl,(posx)
  ld (oldx),hl
  ld a,(posy)
  ld (oldy),a

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

move:
  ; x += dx, bounce at both ends. A negative x wraps to a large unsigned
  ; value, so the high-bit test catches the left edge before the compare.
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
  ld a,-2
  ld (dx),a

move_y:
  ld a,(posy)
  ld b,a
  ld a,(dy)
  add a,b
  jr c,hit_top             ; wrapped below zero
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

; Draw the sprite at (posx, posy), OR-ing so it never erases what it crosses.
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
  or (hl)
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

; Blank the rectangle the sprite occupied last frame.
erase:
  ld a,(oldx)
  ld l,a
  ld a,(oldx+1)
  ld h,a
  srl h
  rr l
  srl h
  rr l
  srl h
  rr l
  ld b,l
  ld a,(oldy)
  call row_addr
  ld a,logo_h
  ld (rowcnt),a
er_row:
  push hl
  ld b,logo_w
er_col:
  ld (hl),0
  inc hl
  djnz er_col
  pop hl
  call next_row
  ld a,(rowcnt)
  dec a
  ld (rowcnt),a
  jr nz,er_row
  ret

include "p2500.inc"

; --- state ------------------------------------------------------------------

posx:   dw 0
posy:   db 0
oldx:   dw 0
oldy:   db 0
dx:     db 1
dy:     db 1
movetick: db 1
xbyte:  db 0
shift:  db 0
sprptr: dw 0
rowcnt: db 0

include "logo_sprite.asm"
