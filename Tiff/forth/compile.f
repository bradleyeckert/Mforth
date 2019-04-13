\ compiler

\ Code and headers are both in flash. Write using SPI!.

: ram  0 c_scope c! ;                   \ use RAM scope
: rom  1 c_scope c! ;                   \ use flash ROM scope
: h   c_scope c@ if CP exit then DP ;   \ scoped pointers
: ,x  dup >r @ SPI!  4 r> +! ;          \ n addr --
: ,c  cp ,x ;                           \ n --
: ,h  hp ,x ;                           \ n --
: ,d  dp dup >r @ !  4 r> +! ;          \ n --
: ,   c_scope c@ if ,c exit then ,d ;   \ n --

: c,x                                   \ c addr --
   >r  hld swap over c!                 \ hld is temporary byte
   r@ @  1 SPImove  1 r> +!             \ program one byte
;
: c,c  cp c,x ;                         \ c --
: c,h  hp c,x ;                         \ c --
: c,d  dp dup >r @ c!  1 r> +! ;        \ c --
: c,   c_scope c@ if c,c exit then c,d ;

: ram  0 c_scope c! ;                   \ use RAM scope
: rom  1 c_scope c! ;                   \ use flash ROM scope

\ Equates for terminal variables puts copies in header space. See tiff.c/tiff.h.
\ Some are omitted to save header space.

handler      equ handler
base         equ base
hp           equ hp
cp           equ cp
dp           equ dp
state        equ state
current      equ current
source-id    equ source-id
personality  equ personality
tibs         equ tibs
tibb         equ tibb
>in          equ >in
c_wids       equ c_wids
c_casesens   equ c_casesens
\ head         equ head
context      equ context
forthwid     equ forthwid
hld          equ hld
blk          equ blk
tib          equ tib
\ maxtibsize   equ maxtibsize
|pad|        equ |pad|
pad          equ pad

\ Define a compiler
\ char params:  c_colondef c_litpend c_slot c_called

defer FlushLit
defer NewGroup

: ClearIR   \ --                        \ Initialize the IR. slot=26, litpend=0
   0 iracc !  26 c_slot w!  0 c_called c!
;
: AppendIR  \ opcode imm --             \ insert opcode at slot, adding imm
   swap  c_slot c@ lshift  iracc @      \ shift it and get IR
   +  +                    iracc !      \ merge into instruction
;

: Implicit  \ opcode --                 \ add implicit opcode to group
   FlushLit
   c_slot c@ 0=  over 60 and  and if    \ doesn't fit in the last slot
      NewGroup
   then
   0 AppendIR
   c_slot c@  dup if
      6 -  dup 0< invert and            \ sequence: 26, 20, 14, 8, 2, 0
      c_slot c! exit
   then
   drop NewGroup                        \ slot 0 -> slot 26
;

: Explicit  \ imm opcode --
   swap FlushLit
   1  c_slot c@  dup >r  lshift         \ opcode imm maximum | slot
   over <  r> 0= or                     \ imm doesn't fit of slot=0
   if  NewGroup  then
   AppendIR
   c_slot c@  24 lshift
   cp @ +  calladdr !                   \ remember call address in case of ;
   0 c_slot c!  NewGroup
;

: HardLit  \ n --                       \ compile a hard literal
   dup >r -if: negate |                 \ u
   dup 33554431 invert 2* and if        \ too wide
      r> drop   dup 24 rshift  op_lit Explicit   \ upper part
      16777215 and  op_litx Explicit    \ lower part
      exit
   then
   r> 0< if                             \ compile negative
      1- op_lit Explicit
      op_com Implicit  exit
   then
   op_lit Explicit                      \ compile positive
;

:noname \ FlushLit  \ --                \ compile a literal if it's pending
   c_litpend c@ if
      0 c_litpend c!
      nextlit @ HardLit
   then
   0 c_called c!                        \ everything clears the call tail
; is FlushLit

:noname \ NewGroup  \ --                \ finish the group and start a new one
   FlushLit                             \ if a literal is pending, compile it
   c_slot c@
   dup 21 - +if: 2drop exit |           \ already at first slot
   drop if
      op_no: Implicit                   \ skip unused slots
   then
   iracc @ ,c
   ClearIR
; is NewGroup

: literal,  \ n --                      \ compile a literal
   FlushLit
   dup 33554431 invert 2* and
   if HardLit exit then
   nextlit !  1 c_litpend c!
;
: compile,  \ xt --                     \ compile a call
   2/ 2/ op_call Explicit
   head @ 4 + c@  128 and  c_called c!  \ 0 = call-only
;
: ,exit  \ --
   c_called c@ if
      calladdr link>
      8  [ calladdr 3 + ] literal c@  lshift
      invert swap SPI!                  \ convert CALL to JUMP
      0 c_called c!
      0 calladdr ! exit
   then op_exit Implicit
;
