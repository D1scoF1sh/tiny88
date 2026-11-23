TINY 2.1/88 - Tiny-2.1 for the IBM PC 5150 (8088, 4.77 MHz, no 8087)
=====================================================================

Tiny-2.1 (github.com/WollyMonjac/Tiny-2.1) is a 367,000-weight neural network
that answers questions letter by letter.  The original needs a 486DX: it uses
floating point, and emulated floating point on an 8088 would take hours per
answer.  This port runs the same network in integer arithmetic, with the
multiply loops in 8088 assembly.  Checked against the original on 160
questions: 145 answers word-for-word identical, and the same accuracy.

Files (keep them together, e.g. C:\TINY88)
  TINY88.EXE   the program
  TINY88.BIN   the network (Tiny-2.1's TINY.BIN converted, 344 KB)
  TINY0.BIN    layer 0 precomputed per letter and per position (145 KB)
  TINYPFX.BIN  memory of 30,000 common question beginnings (25 MB, optional)

Run
  CD \TINY88
  TINY88

Speed on a 4.77 MHz 8088
  Every letter the network reads or writes is one pass through all its
  weights: about 5-8 seconds.  Common question beginnings ("what is the
  capital of ...") come from TINYPFX.BIN instead, which is much faster.
  Typical answers take 1 to 5 minutes; "what is the capital of peru" takes
  well under a minute.  Press Esc while it thinks to stop.

Screens
  CGA   colour, with snow-free writes for the original IBM CGA card
  MDA   automatically uses the monochrome attributes
  /DUAL with both a CGA and an MDA installed: the chat on the main screen,
        a full-screen live NEURAL MONITOR on the other one (layer activity,
        the 4 best guesses for the next letter, timing, and where each
        layer's attention is looking in the text).

Options (can be combined: TINY88 0 /DUAL)
  TINY88 0      always the surest answer (best for quizzes and sums)
  TINY88 5      default: answers worded differently each time
  TINY88 9      the most varied (and more mistakes)
  /DUAL         use both screens (see above)
  /MONO         grey colours on a colour screen
  /QUIET        no sound
  /DEMO         TINY asks itself questions; a key takes over
  /P            plain text; also used for TINY88 < QUESTIONS.TXT > ANSWERS.TXT
  /NOSNOW       fast screen writes (CGA clones, EGA, VGA do not show snow)
  /NOMEM        do not use TINYPFX.BIN
  /FAST         skip the boot animation

Keys    F1 help  F2 sound  F3 robot voice  F4 creativity  Up = last question
        Esc quits (or stops an answer while TINY is thinking)
Words   HELP, CLS, QUIT

Memory: about 520 KB free conventional memory is needed (MEM shows it).

How it works
  - Weights are bytes with one scale per row, as in TINY.BIN.  Every vector
    going into a weight table is turned into bytes too; sums are exact
    32-bit integers; scales are 16-bit mantissa + exponent.  No floats.
  - Inner loop: LODSB / MUL BYTE [BP+n] / ADD BX,AX / ADC DL,CH - unsigned
    multiplies (faster than IMUL on the 8088) with the offsets corrected
    per row.
  - Layer 0's keys, values and queries are split into a letter part and a
    position part and read from TINY0.BIN, so those weights are not needed.
  - For letters of the question only keys and values are needed in the last
    layer, so the rest of that layer is skipped.
  - TINYPFX.BIN holds the network's memory (keys and values of every layer)
    for common question beginnings, computed in advance with the same
    integer arithmetic, so the result is identical, only faster.
