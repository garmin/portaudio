# Make PortAudio for Silicon Graphics IRIX 6.2 - 6.5.
# Pieter suurmond, march 13, 2003. (pa-V18-patch).
# Tested again under IRIX 6.5 with both GCC and MIPS compilers.
# Based on SGI-specific sproc()-method to spawn children, not on POSIX-threads.

# Instead of "-lpthread", as with linux, just the audio- and math-library for SGI:
LIBS = -laudio -lm

CDEFINES = -I../pa_common

# Possible CFLAGS with MIPSpro compiler are: -32, -o32, -n32, -64,
# -mips1, -mips2, -mips3, -mips4, etc. Use -g, -g2, -g3 for debugging. 
# For GCC, use -Wall.
# And use for example -O2 or -O3 for better optimization:
CFLAGS = -O2 -Wall
PASRC = ../pa_common/pa_lib.c pa_sgi.c
PAINC = ../pa_common/portaudio.h

# Tests performed on SGI Indy with R5000 @ 180MHz running IRIX 6.5:
# Used GCC compiler version 3.0.4. and MIPS.

#TESTC = $(PASRC) ../pa_tests/patest_record.c    ## OK GCC march 2003 (MIPS sees errors in patest_record.c, refuses compilation).
#TESTC = $(PASRC) ../pa_tests/patest_latency.c   ## OK GCC march 2003. (MIPS doesn't like //)
#TESTC = $(PASRC) ../pa_tests/patest_longsine.c  ## OK GCC march 2003.
#TESTC = $(PASRC) ../pa_tests/patest_wire.c      ## OK GCC Click free now. march 2003.
#TESTC = $(PASRC) ../pa_tests/pa_fuzz.c          ## OK GCC march 2003.

#TESTC = $(PASRC) ../pa_tests/pa_devs.c          # Never knew my Indy had 16 input- and output-channels!
#TESTC = $(PASRC) ../pa_tests/patest_saw.c       ## -  Pa_sleep doesn't work anymore?!
#TESTC = $(PASRC) ../pa_tests/patest_sine.c      # -  Pa_sleep doesn't work anymore?!
#TESTC = $(PASRC) ../pa_tests/patest_many.c      ## -  Pa_sleep doesn't work anymore?!
#TESTC = $(PASRC) ../pa_tests/patest_sine_time.c ## Silence for 2 seconds doesn't work, sine sounds ok.

#TESTC = $(PASRC) ../pa_tests/patest_sine8.c     ## Silence for 2 seconds doesn't work, sine sounds ok.
#TESTC = $(PASRC) ../pa_tests/patest_leftright.c # OK GCC and MIPS-CC march 2003.
#TESTC = $(PASRC) ../pa_tests/patest_pink.c      ## OK GCC march 2003
#TESTC = $(PASRC) ../pa_tests/patest_clip.c      # 
#TESTC = $(PASRC) ../pa_tests/patest_stop.c      # MIPS doesn't like patest_stop.c Worked before but now
                                                 # error AL_BAD_QSIZE on IRIX 6.5 (with GCC).
#TESTC = $(PASRC) ../pa_tests/patest_dither.c    # 
#TESTC = $(PASRC) ../pa_tests/patest_sync.c      # I don't hear the 6th beep, not really in sync @500ms lat.
#TESTC = $(PASRC) ../pa_tests/paqa_devs.c        # A lot of error messages but no coredump.
#TESTC = $(PASRC) ../pa_tests/paqa_errs.c        # Segmentation fault (core dumped)!

TESTH   = $(PAINC)

all: patest

# "cc" for the MIPSpro compiler, may be changed to "gcc" for the GNU compiler:
patest: $(TESTC) $(TESTH) Makefile
	gcc $(CFLAGS) $(TESTC) $(CDEFINES) $(LIBS) -o patest

run: patest
	./patest

