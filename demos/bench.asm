; ---------------------------------------------------------------------------
; BENCH.COM - small P2500/P2000B benchmark.
;
; Every figure here is a wall-clock measurement against CBIOS's 50 Hz tick
; counter, so the same binary gives comparable numbers on the emulator and on
; real hardware. Where they disagree, the emulator is wrong - and the most
; likely reason is video RAM: it charges a write to the video window exactly
; what it charges main RAM, while a real video card shares that DRAM with the
; CRTC's display fetches and may well stall the CPU during active display.
; The VRAM rows against the RAM rows are the measurement that settles it.
;
; Results are collected first and printed afterwards, because console output
; costs about a millisecond a character on this machine and would otherwise
; be inside the timings.
;
; Interrupts stay ENABLED throughout - they have to, since the clock they are
; measured against is one - so every figure includes the 50 Hz ISR. That is
; the same on both sides and is a fraction of a percent.
; ---------------------------------------------------------------------------

org $0100

BDOS      equ $0005
TICKS     equ $F436          ; CBIOS's 50 Hz counter
PORT_BANK equ $05
BANK_VID  equ $08
BANK_MAIN equ $0F
VRAM      equ $8000
BUF       equ $4000          ; 16 KB of main RAM, clear of code and stack
BLOCK     equ $4000          ; bytes touched per pass
ESC       equ 27

; Work per test. Chosen so each runs somewhere near a second on a 4 MHz
; machine - long enough that 20 ms of tick resolution is under 2%.
PASSES_W  equ 24             ; x 16 KB written
PASSES_R  equ 24             ; x 16 KB read
PASSES_C  equ 12             ; x 16 KB copied
CPU_LOOPS equ 3              ; x 65536 register iterations
CHARS     equ 512            ; characters through BDOS 2
DOTS      equ 192            ; dots through the firmware's set-point call
RECORDS   equ 64             ; 128-byte records read from a file

start:
  ld sp,stack

  ld hl,t_cpu
  call timed
  ld hl,t_ramw
  call timed
  ld hl,t_ramr
  call timed
  ld hl,t_ramc
  call timed
  ld hl,t_vidw
  call timed
  ld hl,t_vidc
  call timed

  ; Graphics mode reprograms the CRTC - 64 columns of 4 scanlines instead of
  ; 80 of 12 - so it fetches display memory on a different rhythm. If the
  ; card steals cycles from the CPU, these two need not match the text rows.
  call gfx_on
  ld hl,t_gvidw
  call timed
  ld hl,t_gvidc
  call timed
  ld hl,t_plot
  call timed
  call gfx_off

  ld hl,t_conout
  call timed
  ld hl,t_disk
  call timed

  call show
  ret

; --- the harness ------------------------------------------------------------

; hl -> a test record: dw body, db name(12), dw result
timed:
  ld (cur),hl
  ld de,0
  add hl,de
  ld e,(hl)
  inc hl
  ld d,(hl)
  ld (body),de
  call read_ticks
  ld (t0),hl
  call go
  call read_ticks
  ld de,(t0)
  or a
  sbc hl,de
  ex de,hl
  ld hl,(cur)
  ld bc,14
  add hl,bc                  ; -> result field
  ld (hl),e
  inc hl
  ld (hl),d
  ret
go:
  ld hl,(body)
  jp (hl)

; The counter is three bytes and an interrupt can land between our reads, so
; take the high byte twice and retry if it moved.
read_ticks:
  ld a,(TICKS+1)
  ld h,a
  ld a,(TICKS)
  ld l,a
  ld a,(TICKS+1)
  cp h
  jr nz,read_ticks
  ret

vid_in:
  ld a,BANK_VID
  out (PORT_BANK),a
  ret
vid_out:
  ld a,BANK_MAIN
  out (PORT_BANK),a
  ret

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

; --- the tests --------------------------------------------------------------

; Registers only: no data memory touched, so this is instruction fetch and
; execution alone. If this row differs, nothing else can be compared.
; The pass counter lives in memory, not in A. A is needed for `ld a,b / or c`
; inside the loop, and saving it around that with push/pop af would restore
; the flags as well - wiping the very test the branch depends on.
b_cpu:
  ld a,CPU_LOOPS
  ld (passes),a
cpu_outer:
  ld bc,0
  ld de,1
  ld hl,0
cpu_inner:
  add hl,de
  dec bc
  ld a,b
  or c
  jr nz,cpu_inner
  call next_pass
  jr nz,cpu_outer
  ret

next_pass:
  ld a,(passes)
  dec a
  ld (passes),a
  ret

; 16 KB of byte writes, unrolled eight deep so the loop overhead does not
; drown the memory access.
b_ramw:
  ld a,PASSES_W
  ld (passes),a
ramw_pass:
  ld hl,BUF
  ld bc,BLOCK/8
  ld e,$55
ramw_loop:
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  dec bc
  ld a,b
  or c
  jr nz,ramw_loop
  call next_pass
  jr nz,ramw_pass
  ret

b_ramr:
  ld a,PASSES_R
  ld (passes),a
ramr_pass:
  ld hl,BUF
  ld bc,BLOCK/8
ramr_loop:
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  ld d,(hl)
  inc hl
  dec bc
  ld a,b
  or c
  jr nz,ramr_loop
  call next_pass
  jr nz,ramr_pass
  ret

; LDIR reads and writes on every iteration, so it stresses the bus twice as
; hard per byte as the plain loops do.
b_ramc:
  ld a,PASSES_C
ramc_pass:
  push af
  ld hl,BUF
  ld de,BUF+1
  ld bc,BLOCK-1
  ldir
  pop af
  dec a
  jr nz,ramc_pass
  ret

b_vidw:
  call vid_in
  call vw_body
  jp vid_out
vw_body:
  ld a,PASSES_W
  ld (passes),a
vw_pass:
  ld hl,VRAM
  ld bc,BLOCK/8
  ld e,$55
vw_loop:
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  ld (hl),e
  inc hl
  dec bc
  ld a,b
  or c
  jr nz,vw_loop
  call next_pass
  jr nz,vw_pass
  ret

b_vidc:
  call vid_in
  call vc_body
  jp vid_out
vc_body:
  ld a,PASSES_C
vc_pass:
  push af
  ld hl,VRAM
  ld de,VRAM+1
  ld bc,BLOCK-1
  ldir
  pop af
  dec a
  jr nz,vc_pass
  ret

; Dots through CBIOS's own set-point call: ESC 3 armed it, and four bytes
; through the console per dot is the documented way to draw.
b_plot:
  ld b,DOTS
plot_loop:
  push bc
  ld e,1
  call conout
  ld e,0
  call conout
  ld e,0
  call conout
  ld e,0
  call conout
  pop bc
  djnz plot_loop
  ret

b_conout:
  ld bc,CHARS
co_loop:
  push bc
  ld e,'.'
  call conout
  pop bc
  dec bc
  ld a,b
  or c
  jr nz,co_loop
  ret

; A sequential read of a file that is on every bootable P2500 disk. The
; emulator hands a whole sector over in one memcpy with no seek and no
; rotational latency, so this row is the one most likely to disagree.
b_disk:
  ld de,fcb
  ld c,15                    ; open
  call BDOS
  inc a
  ret z                      ; no such file - leave the result at zero
  ld bc,RECORDS
disk_loop:
  push bc
  ld de,fcb
  ld c,20                    ; read sequential
  call BDOS
  pop bc
  or a
  ret nz                     ; end of file
  dec bc
  ld a,b
  or c
  jr nz,disk_loop
  ret

; --- reporting --------------------------------------------------------------

show:
  ; Home and clear: the console test has just scrolled 512 dots past.
  ld e,ESC
  call conout
  ld e,'Y'
  call conout
  ld e,' '
  call conout
  ld e,' '
  call conout
  ld e,ESC
  call conout
  ld e,'k'
  call conout
  ld de,header
  ld c,9
  call BDOS
  ld hl,t_cpu
  ld b,11
show_row:
  push bc
  push hl
  push hl
  ld de,2
  add hl,de
  ex de,hl
  ld c,9
  call BDOS                  ; name
  pop hl
  ld de,14
  add hl,de
  ld e,(hl)
  inc hl
  ld d,(hl)
  ex de,hl
  call print_u16
  ld de,crlf_s
  ld c,9
  call BDOS
  pop hl
  ld de,16
  add hl,de
  pop bc
  djnz show_row
  ld de,footer
  ld c,9
  jp BDOS

; hl unsigned -> five digits, leading zeros blanked. `lead` has to be reset
; per number, or every row after the first prints its zeros.
print_u16:
  xor a
  ld (lead),a
  ld de,10000
  call digit
  ld de,1000
  call digit
  ld de,100
  call digit
  ld de,10
  call digit
  ld a,l
  add a,'0'
  ld e,a
  ld (lead),a                ; the last digit always prints
  jp conout
digit:
  ld b,'0'-1
dg1:
  inc b
  or a
  sbc hl,de
  jr nc,dg1
  add hl,de
  ld a,b
  cp '0'
  jr nz,dg_print
  ld a,(lead)
  or a
  jr nz,dg_zero
  ld e,' '
  jp conout
dg_zero:
  ld a,'0'
dg_print:
  ld (lead),a
  ld e,a
  jp conout

lead: db 0

; --- test table -------------------------------------------------------------
; dw body, db 12-char name terminated by '$', dw result

t_cpu:    dw b_cpu
          db "CPU regs   $"
          dw 0
t_ramw:   dw b_ramw
          db "RAM write  $"
          dw 0
t_ramr:   dw b_ramr
          db "RAM read   $"
          dw 0
t_ramc:   dw b_ramc
          db "RAM ldir   $"
          dw 0
t_vidw:   dw b_vidw
          db "VID write T$"
          dw 0
t_vidc:   dw b_vidc
          db "VID ldir  T$"
          dw 0
t_gvidw:  dw b_vidw
          db "VID write G$"
          dw 0
t_gvidc:  dw b_vidc
          db "VID ldir  G$"
          dw 0
t_plot:   dw b_plot
          db "Firmware  .$"
          dw 0
t_conout: dw b_conout
          db "Console   .$"
          dw 0
t_disk:   dw b_disk
          db "Disk read .$"
          dw 0

header:
  db 13,10,"P2500 BENCH - ticks at 50 Hz, so x20 = ms",13,10
  db "T = text mode, G = graphics mode",13,10,13,10,"$"
footer:
  db 13,10,"RAM 24x16K wr/rd, 12x16K ldir, CPU 3x65536,",13,10
  db "192 dots, 512 chars, 64 records.",13,10,"$"
crlf_s:
  db 13,10,"$"

fcb:
  db 0,"SYSCPM  PHI",0,0,0,0
  ds 16,0
  db 0,0,0,0

passes: db 0
cur:  dw 0
body: dw 0
t0:   dw 0

  ds 64,0
stack:
