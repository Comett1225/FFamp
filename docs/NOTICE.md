# Notices

FFamp files whose code or tables are based on FFmpeg, one line each: the FFamp file, the FFmpeg files it is based on, and their
copyright holders. Those FFmpeg files are licensed LGPL-2.1-or-later.

- `src/wma_decode.cpp`: `libavcodec/wmadec.c`, `libavcodec/wma.c`, `libavcodec/wma.h`; Copyright (c) 2002-2007 The FFmpeg Project; LGPL-2.1-or-later.
- `src/wma_common.cpp`: `libavcodec/wma.c`, `libavcodec/wma_common.c`; Copyright (c) 2002-2007 The FFmpeg Project (`wma_common.c` names no holder); LGPL-2.1-or-later.
- `src/wma.h`: `libavcodec/wma.h`; Copyright (c) 2002-2007 The FFmpeg Project; LGPL-2.1-or-later.
- `src/wma_tables.cpp`: `libavcodec/wmadata.h`, `libavcodec/wmadec.c`, `libavcodec/wma_freqs.c`, `libavcodec/aactab.c`; Copyright (c) 2002 The FFmpeg Project, Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov (`wma_freqs.c` names no holder); LGPL-2.1-or-later.
- `src/mdct.cpp`: `libavutil/tx_template.c`, `libavcodec/sinewin_tablegen.h`, `libavcodec/kbdwin.c`, `libavutil/mathematics.c` (the Bessel function of the KBD window); Copyright (c) Lynne, Copyright (c) 2008 Loren Merritt, Copyright (c) 2002 Fabrice Bellard, Copyright (c) 2009 Reimar Döffinger, Copyright (c) 2005-2012 Michael Niedermayer (`kbdwin.c` names no holder); LGPL-2.1-or-later.
- `src/vlc.cpp`: `libavcodec/vlc.c`; Copyright (c) 2000, 2001 Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer, Copyright (c) 2010 Loren Merritt; LGPL-2.1-or-later.
- `src/wmapro_decode.cpp`: `libavcodec/wmaprodec.c`, `libavcodec/wmaprodata.h`, `libavutil/float_dsp.c`; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson (`wmaprodata.h`: Copyright (c) 2008-2009 Sascha Sommer), Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt; LGPL-2.1-or-later.
- `tests/fixtures/wmapro/make.py`: writes the bitstream `libavcodec/wmaprodec.c` reads, with the tables of `src/wmapro_decode.cpp`; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson; LGPL-2.1-or-later.
- `src/vc1.h`: libavcodec/vc1.h, vc1_common.h, vc1data.h, vc1_pred.h, mathops.h, get_bits.h, unary.h; Copyright (c) 2006-2007 Konstantin Shishkov, partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer; Copyright (c) 2004 Michael Niedermayer; Copyright (c) 2001, 2002 Fabrice Bellard; Copyright (c) 2006 Michael Niedermayer et al. LGPL-2.1-or-later.
- `src/vc1_tables.h`, `src/vc1_tables.cpp`: libavcodec/vc1_vlc_data.h, vc1acdata.h, vc1data.c, msmpeg4_vc1_data.c, wmv2data.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006 Konstantin Shishkov; (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer; Copyright (c) 2001 Fabrice Bellard; Copyright (c) 2002-2004 Michael Niedermayer; the FFmpeg project (wmv2data.c names no holder). LGPL-2.1-or-later.
- `src/vc1_header.cpp`: libavcodec/vc1.c, vc1data.c, vc1dec.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer. LGPL-2.1-or-later.
- `src/vc1_block.cpp`: libavcodec/vc1_block.c, mpegvideo.c, mpegvideo.h; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer; Copyright (c) 2000, 2001, 2002 Fabrice Bellard; Copyright (c) 2002-2004 Michael Niedermayer. LGPL-2.1-or-later.
- `src/vc1_pred.cpp`: libavcodec/vc1_pred.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer. LGPL-2.1-or-later.
- `src/vc1_mc.cpp`: libavcodec/vc1_mc.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer. LGPL-2.1-or-later.
- `src/vc1_filter.cpp`: libavcodec/vc1_loopfilter.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer. LGPL-2.1-or-later.
- `src/vc1_dsp.cpp`: libavcodec/vc1dsp.c, vc1_common.h, hpeldsp.c, h264chroma_template.c, idctdsp.c, simple_idct.c, simple_idct_template.c, videodsp_template.c, aarch64/simple_idct_neon.S; Copyright (c) 2008 Mans Rullgard; Copyright (c) 2017 Matthieu Bouron; Copyright (c) 2006 Konstantin Shishkov; Copyright (c) 2006-2007 Konstantin Shishkov, partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer; Copyright (c) 2000, 2001 Fabrice Bellard; Copyright (c) 2002-2004 Michael Niedermayer; Copyright (c) 2001 Michael Niedermayer; Copyright (c) 2002-2012 Michael Niedermayer; Copyright (C) 2012 Ronald S. Bultje; the FFmpeg project (idctdsp.c names no holder). LGPL-2.1-or-later.
- `src/vc1_decode.cpp`: libavcodec/vc1dec.c, mpegvideo.c, mpegvideo_dec.c, mpegpicture.c; Copyright (c) 2011 Mashiat Sarker Shakkhar; Copyright (c) 2006-2007 Konstantin Shishkov; partly based on vc9.c (c) 2005 Anonymous, Alex Beregszaszi, Michael Niedermayer; Copyright (c) 2000, 2001 Fabrice Bellard; Copyright (c) 2002-2004 Michael Niedermayer; the FFmpeg project (mpegpicture.c names no holder). LGPL-2.1-or-later.
- `src/wmalossless_decode.cpp`: `libavcodec/wmalosslessdec.c`, `libavcodec/lossless_audiodsp.c`; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson, Copyright (c) 2011 Andreas Öman, Copyright (c) 2011-2012 Mashiat Sarker Shakkhar, Copyright (c) 2007 Benjamin Zores (`lossless_audiodsp.c`, based upon libdemac from Dave Chapman); LGPL-2.1-or-later.
- `tests/fixtures/wmalossless/make.py`: writes the bitstream `libavcodec/wmalosslessdec.c` reads; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson, Copyright (c) 2011 Andreas Öman, Copyright (c) 2011-2012 Mashiat Sarker Shakkhar; LGPL-2.1-or-later.
- `src/h263.h`, `src/h263.cpp`: libavcodec/mpegvideo.c, mpegvideo_dec.c, mpegvideo_motion.c, mpegvideo_unquantize.c, h263.c, h263dsp.c, ituh263dec.c, mpeg4video.c, mpeg4videodec.c, msmpeg4.c, rl.c, x86/hpeldsp.asm (the approximate no-rounding half-pel averages); Copyright (c) 2000, 2001 Fabrice Bellard; Copyright (c) 2001 Juan J. Sierralta P; Copyright (c) 2002-2010 Michael Niedermayer; Copyright (c) Nick Kurshev; Copyright (c) 2002 Zdenek Kabelac; Copyright (c) 2013 Daniel Kang; the FFmpeg project (h263dsp.c, rl.c name no holder). LGPL-2.1-or-later.
- `src/msmpeg4.cpp`: libavcodec/msmpeg4dec.c, msmpeg4.c, wmv2dec.c, wmv2.h, h263dec.c, qpeldsp.c, mathtables.c; Copyright (c) 2001 Fabrice Bellard; Copyright (c) 2002-2013 Michael Niedermayer; Copyright (c) 2002 The FFmpeg Project. LGPL-2.1-or-later.
- `src/msmpeg4_tables.cpp`: libavcodec/h263data.c, h263dsp.c, mpeg4data.h, mpegvideodata.c, mathtables.c, msmpeg4data.c, intrax8huf.h; Copyright (c) 2000, 2001 Fabrice Bellard; Copyright (c) 2002-2004 Michael Niedermayer; the FFmpeg project (h263data.c, h263dsp.c, mpegvideodata.c, intrax8huf.h name no holder). LGPL-2.1-or-later.
- `src/intrax8.h`, `src/intrax8.cpp`: libavcodec/intrax8.c, intrax8.h, intrax8dsp.c, wmv2dsp.c; the FFmpeg project (these files name no holder). LGPL-2.1-or-later.
- `src/wmavoice_decode.cpp`: `libavcodec/wmavoice.c`; Copyright (c) 2009 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/wmavoice_tables.cpp`: `libavcodec/wmavoice_data.h` (converted by script); Copyright (c) 2009 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/wmavoice_dsp.cpp`: `libavcodec/celp_filters.c`, `libavcodec/acelp_vectors.c`, `libavcodec/acelp_filters.c`, `libavcodec/lsp.c`, `libavutil/float_scalarproduct.c`, `libavutil/tx_template.c`, `libavutil/tx.c`, `libavutil/tx_priv.h`; Copyright (c) 2008 Vladimir Voroshilov, Copyright (c) 2007 Reynaldo H. Verdejo Pinochet, Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt, Copyright (c) Lynne, Copyright (c) 2008 Loren Merritt, Copyright (c) 2002 Fabrice Bellard, partly based on libdjbfft by D. J. Bernstein (`tx.c` and `tx_priv.h` name no holder); LGPL-2.1-or-later.
- `src/wmavoice.h`: `libavcodec/wmavoice_data.h`, `libavcodec/acelp_vectors.h`; Copyright (c) 2009 Ronald S. Bultje, Copyright (c) 2008 Vladimir Voroshilov; LGPL-2.1-or-later.
- `tests/fixtures/wmavoice/make.py`: writes the bitstream `libavcodec/wmavoice.c` reads, with the tables of `src/wmavoice_decode.cpp`; Copyright (c) 2009 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/wave_audio.cpp`: `libavcodec/adpcm.c`, `libavcodec/adpcm_data.c`, `libavcodec/adpcm_data.h`, `libavcodec/pcm_tablegen.h`, `libavcodec/g726.c`, `libavcodec/g722.c`, `libavcodec/g722dec.c`, `libavcodec/g722dsp.c`; Copyright (c) 2001-2003 The FFmpeg project, Copyright (c) 2010 Reimar Döffinger, Copyright (c) 2004 Roman Shaposhnik, Copyright (c) CMU 1993 Computer Science, Speech Group, Copyright (c) 2005 Steve Underwood, Copyright (c) 2009 Kenan Gillet, Copyright (c) 2010 Martin Storsjo, Copyright (c) 2015 Peter Meerwald; LGPL-2.1-or-later.
- `src/wav_read.cpp`: `libavformat/wavdec.c` (the block size of Microsoft ADPCM with more than two channels); Copyright (c) 2001, 2002 Fabrice Bellard, Copyright (c) 2009 Daniel Verkamp; LGPL-2.1-or-later.
- `src/mp4_read.cpp`: `libavformat/mov.c` (how QuickTime sound counted in frames is cut into packets, how several edits are joined and where the first video picture of an edit is shown, the composition offset shift of fragments), `libavformat/mov_chan.c` (the channel layout tags of the `chan` box); Copyright (c) 2001 Fabrice Bellard, Copyright (c) 2009 Baptiste Coudurier, Copyright (c) 2011 Justin Ruggles; LGPL-2.1-or-later.
- `tests/fixtures/sound/make.py`: writes the block layouts `libavcodec/adpcm.c` reads; Copyright (c) 2001-2003 The FFmpeg project; LGPL-2.1-or-later.
- `src/tta_decode.cpp`: `libavcodec/tta.c`, `libavcodec/ttadata.c`, `libavcodec/ttadsp.c`, `libavcodec/unary.h`; Copyright (c) 2006 Alex Beregszaszi, Copyright (c) 2004 Michael Niedermayer (`unary.h`; `ttadata.c` and `ttadsp.c` name no holder); LGPL-2.1-or-later.
- `src/mkv_read.cpp`: `libavformat/matroskadec.c` (the TTA1 header built for a TTA track, the rotation from the Projection pose, the FLAC channel mask comment), `libavcodec/vorbis_parser.c` (the length of the first Vorbis packet); Copyright (c) 2003-2008 The FFmpeg Project, Copyright (c) 2012 Justin Ruggles; LGPL-2.1-or-later.
- `src/mkv_write.cpp`: `libavformat/matroskaenc.c` (the Projection pose roll written for a rotation); Copyright (c) 2007 David Conrad; LGPL-2.1-or-later.
- `src/alac_decode.cpp`: `libavcodec/alac.c`, `libavcodec/alac_data.c`; Copyright (c) 2005 David Hammerton; LGPL-2.1-or-later.
- `src/ac3_decode.cpp`: `libavcodec/ac3dec.c`, `libavcodec/ac3dec.h`, `libavcodec/ac3dec_float.c`, `libavcodec/eac3dec.c`, `libavcodec/ac3_parser.c`, `libavcodec/ac3tab.c` and `libavcodec/ac3dec_data.c` (the downmix gain levels), `libavcodec/ac3.c`, `libavcodec/ac3dsp.c`, `libavcodec/kbdwin.c`, `libavcodec/mathops.h`, `libavutil/lfg.c`, `libavutil/mathematics.c`, `libavutil/float_dsp.c`; Copyright (c) 2006 Kartikey Mahendra BHATT, Copyright (c) 2007-2008 Bartlomiej Wolowiec, Copyright (c) 2007 Justin Ruggles, Copyright (c) 2008 Justin Ruggles, Copyright (c) 2003 Fabrice Bellard, Copyright (c) 2003 Michael Niedermayer, Copyright (c) 2000 Fabrice Bellard, Copyright (c) 2011 Justin Ruggles, Copyright (c) 2001, 2002 Fabrice Bellard, Copyright (c) 2006 Michael Niedermayer et al, Copyright (c) 2008 Michael Niedermayer, Copyright (c) 2005-2012 Michael Niedermayer, Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt (`kbdwin.c` names no holder); LGPL-2.1-or-later.
- `src/ac3_tables.h`, `src/ac3_tables.cpp`: `libavcodec/ac3tab.c`, `libavcodec/ac3tab.h`, `libavcodec/ac3.c`, `libavcodec/ac3dec_data.c`, `libavcodec/eac3_data.c`, `libavcodec/ac3_channel_layout_tab.c`, `libavutil/lfg.c` (the generator state it makes from seed 0); Copyright (c) 2001 Fabrice Bellard, Copyright (c) 2000, 2001, 2002 Fabrice Bellard, Copyright (c) 2000 Fabrice Bellard, Copyright (c) 2007 Bartlomiej Wolowiec, Copyright (c) 2008 Michael Niedermayer; LGPL-2.1-or-later.
- `tests/ac3_make.py`: writes the bitstream `libavcodec/ac3dec.c` and `libavcodec/eac3dec.c` read, with the header syntax of `libavcodec/ac3_parser.c` and the bit allocation of `libavcodec/ac3.c` and `libavcodec/ac3dsp.c` (tables read from `src/ac3_tables.cpp`); Copyright (c) 2006 Kartikey Mahendra BHATT, Copyright (c) 2007-2008 Bartlomiej Wolowiec, Copyright (c) 2007 Justin Ruggles, Copyright (c) 2008 Justin Ruggles, Copyright (c) 2003 Fabrice Bellard, Copyright (c) 2003 Michael Niedermayer, Copyright (c) 2000 Fabrice Bellard, Copyright (c) 2011 Justin Ruggles; LGPL-2.1-or-later.
- `src/aac_decode.cpp`: `libavcodec/aac/aacdec.c`, `libavcodec/aac/aacdec.h`, `libavcodec/aac/aacdec_float.c`, `libavcodec/aac/aacdec_dsp_template.c`, `libavcodec/aac/aacdec_proc_template.c`, `libavcodec/aac/aacdec_tab.c`, `libavcodec/aactab.c`, `libavcodec/mpeg4audio.c`, `libavcodec/cbrt_tablegen.h`, `libavcodec/cbrt_tablegen_common.c`, `libavcodec/lpc_functions.h`, `libavutil/float_dsp.c`, `libavutil/x86/float_dsp.asm` (the order of its sums); Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov, Copyright (c) 2008-2013 Alex Converse, Copyright (c) 2008-2010 Paul Kendall, Copyright (c) 2008 Baptiste Coudurier, Copyright (c) 2009 Alex Converse, Copyright (c) 2010 Reimar Döffinger, Copyright (c) 2006 Justin Ruggles, Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt; LGPL-2.1-or-later.
- `src/aac_sbr.cpp`: `libavcodec/aacsbr.c`, `libavcodec/aacsbr_template.c`, `libavcodec/aacsbr.h`, `libavcodec/sbr.h`, `libavcodec/sbrdsp.c`, `libavcodec/sbrdsp_template.c`, `libavcodec/x86/sbrdsp.asm` (the order of its sums), `libavcodec/aacps.c`, `libavcodec/aacps.h`, `libavcodec/aacps_common.c`, `libavcodec/aacps_tablegen.h`, `libavcodec/aacpsdsp_template.c`, `libavcodec/x86/aacpsdsp.asm` (the order of its sums), `libavcodec/aac/aacdec_tab.c`, `libavutil/float_dsp.c`, `libavutil/x86/float_dsp.asm`; Copyright (c) 2008-2009 Robert Swain, Copyright (c) 2009-2010 Alex Converse, Copyright (c) 2010 Alex Converse, Copyright (C) 2012 Christophe Gisquet, Copyright (C) 2015 James Almer, Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov, Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt; LGPL-2.1-or-later.
- `src/aac_tables.cpp`: `libavcodec/aactab.c`, `libavcodec/aac/aacdec_tab.c`, `libavcodec/aacsbrdata.h`, `libavcodec/sbrdsp_template.c`, `libavcodec/aacpsdata.c` (converted by script); Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov, Copyright (c) 2008-2009 Robert Swain, Copyright (c) 2009-2010 Alex Converse, Copyright (c) 2010 Alex Converse; LGPL-2.1-or-later.
- `src/aac.h`: `libavcodec/sbr.h`, `libavcodec/aacps.h`, `libavcodec/aacsbr.h`, `libavcodec/aac_defines.h`, `libavutil/float_dsp.c`; Copyright (c) 2008-2009 Robert Swain, Copyright (c) 2010 Alex Converse, Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt (`aac_defines.h` names no holder); LGPL-2.1-or-later.
- `src/h264/bit_depth_template.c`: `libavcodec/bit_depth_template.c`; the FFmpeg project (`bit_depth_template.c` names no holder); LGPL-2.1-or-later.
- `src/h264/cabac.c`: `libavcodec/cabac.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/cabac.h`: `libavcodec/cabac.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/cabac_functions.h`: `libavcodec/cabac_functions.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/copy_block.h`: `libavcodec/copy_block.h`; the FFmpeg project (`copy_block.h` names no holder); LGPL-2.1-or-later.
- `src/h264/error_resilience.c`: `libavcodec/error_resilience.c`; Copyright (c) 2002-2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/error_resilience.h`: `libavcodec/error_resilience.h`; the FFmpeg project (`error_resilience.h` names no holder); LGPL-2.1-or-later.
- `src/h264/get_bits.h`: `libavcodec/get_bits.h`; Copyright (c) 2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/golomb.c`: `libavcodec/golomb.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/golomb.h`: `libavcodec/golomb.h`; Copyright (c) 2003 Michael Niedermayer, Copyright (c) 2004 Alex Beregszaszi; LGPL-2.1-or-later.
- `src/h264/h264.h`: `libavcodec/h264.h`; the FFmpeg project (`h264.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645_parse.c`: `libavcodec/h2645_parse.c`; the FFmpeg project (`h2645_parse.c` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645_parse.h`: `libavcodec/h2645_parse.h`; the FFmpeg project (`h2645_parse.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645_sei.c`: `libavcodec/h2645_sei.c`; Copyright (c) 2003 Michael Niedermayer, Copyright (C) 2012 - 2013 Guillaume Martres, Copyright (C) 2012 - 2013 Gildas Cocherel, Copyright (C) 2013 Vittorio Giovara; LGPL-2.1-or-later.
- `src/h264/h2645_sei.h`: `libavcodec/h2645_sei.h`; the FFmpeg project (`h2645_sei.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645_vui.c`: `libavcodec/h2645_vui.c`; Copyright (c) 2003 Michael Niedermayer, Copyright (C) 2012 - 2013 Guillaume Martres, Copyright (C) 2012 - 2013 Mickael Raulet, Copyright (C) 2012 - 2013 Gildas Cocherel, Copyright (C) 2013 Vittorio Giovara; LGPL-2.1-or-later.
- `src/h264/h2645_vui.h`: `libavcodec/h2645_vui.h`; the FFmpeg project (`h2645_vui.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645data.c`: `libavcodec/h2645data.c`; the FFmpeg project (`h2645data.c` names no holder); LGPL-2.1-or-later.
- `src/h264/h2645data.h`: `libavcodec/h2645data.h`; the FFmpeg project (`h2645data.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264_cabac.c`: `libavcodec/h264_cabac.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_cavlc.c`: `libavcodec/h264_cavlc.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_direct.c`: `libavcodec/h264_direct.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_loopfilter.c`: `libavcodec/h264_loopfilter.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_mb.c`: `libavcodec/h264_mb.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_mb_template.c`: `libavcodec/h264_mb_template.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_mc_template.c`: `libavcodec/h264_mc_template.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_mvpred.h`: `libavcodec/h264_mvpred.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_parse.c`: `libavcodec/h264_parse.c`; the FFmpeg project (`h264_parse.c` names no holder); LGPL-2.1-or-later.
- `src/h264/h264_parse.h`: `libavcodec/h264_parse.h`; the FFmpeg project (`h264_parse.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264_picture.c`: `libavcodec/h264_picture.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_ps.c`: `libavcodec/h264_ps.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_ps.h`: `libavcodec/h264_ps.h`; the FFmpeg project (`h264_ps.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264_refs.c`: `libavcodec/h264_refs.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_sei.c`: `libavcodec/h264_sei.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264_sei.h`: `libavcodec/h264_sei.h`; the FFmpeg project (`h264_sei.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264_slice.c`: `libavcodec/h264_slice.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264addpx_template.c`: `libavcodec/h264addpx_template.c`; Copyright (c) 2003-2011 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264chroma.c`: `libavcodec/h264chroma.c`; the FFmpeg project (`h264chroma.c` names no holder); LGPL-2.1-or-later.
- `src/h264/h264chroma.h`: `libavcodec/h264chroma.h`; the FFmpeg project (`h264chroma.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264chroma_template.c`: `libavcodec/h264chroma_template.c`; Copyright (c) 2000, 2001 Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264data.c`: `libavcodec/h264data.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264data.h`: `libavcodec/h264data.h`; the FFmpeg project (`h264data.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264dec.c`: `libavcodec/h264dec.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264dec.h`: `libavcodec/h264dec.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264dsp.c`: `libavcodec/h264dsp.c`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264dsp.h`: `libavcodec/h264dsp.h`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264dsp_template.c`: `libavcodec/h264dsp_template.c`; Copyright (c) 2003-2011 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264idct.c`: `libavcodec/h264idct.c`; Copyright (c) 2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264idct.h`: `libavcodec/h264idct.h`; the FFmpeg project (`h264idct.h` names no holder); LGPL-2.1-or-later.
- `src/h264/h264idct_template.c`: `libavcodec/h264idct_template.c`; Copyright (c) 2004-2011 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264pred.c`: `libavcodec/h264pred.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264pred.h`: `libavcodec/h264pred.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264pred_template.c`: `libavcodec/h264pred_template.c`; Copyright (c) 2003-2011 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264qpel.c`: `libavcodec/h264qpel.c`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264qpel.h`: `libavcodec/h264qpel.h`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/h264qpel_template.c`: `libavcodec/h264qpel_template.c`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/hpel_template.c`: `libavcodec/hpel_template.c`; Copyright (c) 2000, 2001 Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/mathops.h`: `libavcodec/mathops.h`; Copyright (c) 2001, 2002 Fabrice Bellard, Copyright (c) 2006 Michael Niedermayer et al; LGPL-2.1-or-later.
- `src/h264/mathtables.c`: `libavcodec/mathtables.c`; Copyright (c) 2002-2004 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/mpegutils.h`: `libavcodec/mpegutils.h`; the FFmpeg project (`mpegutils.h` names no holder); LGPL-2.1-or-later.
- `src/h264/pel_template.c`: `libavcodec/pel_template.c`; the FFmpeg project (`pel_template.c` names no holder); LGPL-2.1-or-later.
- `src/h264/pixels.h`: `libavcodec/pixels.h`; the FFmpeg project (`pixels.h` names no holder); LGPL-2.1-or-later.
- `src/h264/rectangle.h`: `libavcodec/rectangle.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/rnd_avg.h`: `libavcodec/rnd_avg.h`; Copyright (c) 2001-2003 BERO, Copyright (c) 2011 Oskar Arvidsson; LGPL-2.1-or-later.
- `src/h264/sei.h`: `libavcodec/sei.h`; the FFmpeg project (`sei.h` names no holder); LGPL-2.1-or-later.
- `src/h264/startcode.c`: `libavcodec/startcode.c`; Copyright (c) 2003-2010 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/startcode.h`: `libavcodec/startcode.h`; the FFmpeg project (`startcode.h` names no holder); LGPL-2.1-or-later.
- `src/h264/videodsp.c`: `libavcodec/videodsp.c`; Copyright (C) 2012 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/videodsp.h`: `libavcodec/videodsp.h`; Copyright (C) 2012 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/videodsp_template.c`: `libavcodec/videodsp_template.c`; Copyright (c) 2002-2012 Michael Niedermayer, Copyright (C) 2012 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/vlc.c`: `libavcodec/vlc.c`; Copyright (c) 2000, 2001 Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer, Copyright (c) 2010 Loren Merritt; LGPL-2.1-or-later.
- `src/h264/vlc.h`: `libavcodec/vlc.h`; the FFmpeg project (`vlc.h` names no holder); LGPL-2.1-or-later.
- `src/h264/ffbase.h`, `src/h264/ffbase.c`: `libavutil/common.h`, `intreadwrite.h`, `intmath.h`, `mem.c`, `rational.c`, `buffer.c`, `frame.c`, `refstruct.c`, `pixdesc.c`, `imgutils.c`, `libavcodec/bytestream.h`, `decode.c`; copyright (c) 2006 Michael Niedermayer, Copyright (c) 2010 Mans Rullgard, Copyright (c) 2002 Fabrice Bellard, Copyright (c) 2003 Michael Niedermayer, Copyright (c) 2009 Michael Niedermayer, copyright (c) 2006 Baptiste Coudurier, Copyright (c) 2012 Aneesh Dogra (`buffer.c`, `frame.c`, `refstruct.c`, `imgutils.c`, `decode.c` name no holder); LGPL-2.1-or-later.
- `src/h264/h264_ffamp.c`, `src/h264/h264_threads.h`: `libavcodec/pthread_frame.c`, `pthread_slice.c`, `decode.c`, `libavutil/frame.c` (frame threads, slice threads, frame buffers, cropping); the FFmpeg project (these files name no holder); LGPL-2.1-or-later.
- `src/h264/x86/asm.h`: `libavutil/x86/asm.h`; copyright (c) 2006 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/x86/cabac.h`: `libavcodec/x86/cabac.h`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/x86/constants.c`: `libavcodec/x86/constants.c`; the FFmpeg project (`constants.c` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/constants.h`: `libavcodec/x86/constants.h`; the FFmpeg project (`constants.h` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/cpu.c`: `libavutil/x86/cpu.c`; (c)1997-99 by H. Dietz and R. Fisher; LGPL-2.1-or-later.
- `src/h264/x86/cpu.h`: `libavutil/x86/cpu.h`; the FFmpeg project (`cpu.h` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/cpuid.asm`: `libavutil/x86/cpuid.asm`; Copyright (C) 2005-2010 x264 project (authors: Loren Merritt, Fiona Glaser); LGPL-2.1-or-later.
- `src/h264/x86/fpel.asm`: `libavcodec/x86/fpel.asm`; Copyright (c) 2008 Loren Merritt, Copyright (c) 2003-2013 Michael Niedermayer, Copyright (c) 2013 Daniel Kang; LGPL-2.1-or-later.
- `src/h264/x86/fpel.h`: `libavcodec/x86/fpel.h`; the FFmpeg project (`fpel.h` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/h264_cabac.c`: `libavcodec/x86/h264_cabac.c`; Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/h264/x86/h264_chromamc.asm`: `libavcodec/x86/h264_chromamc.asm`; Copyright (c) 2005 Zoltan Hidvegi, 2005-2008 Loren Merritt; LGPL-2.1-or-later.
- `src/h264/x86/h264_chromamc_10bit.asm`: `libavcodec/x86/h264_chromamc_10bit.asm`; Copyright (C) 2005-2011 x264 project (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264_deblock.asm`: `libavcodec/x86/h264_deblock.asm`; Copyright (C) 2005-2011 x264 project (authors: Loren Merritt, Fiona Glaser, Oskar Arvidsson); LGPL-2.1-or-later.
- `src/h264/x86/h264_deblock_10bit.asm`: `libavcodec/x86/h264_deblock_10bit.asm`; Copyright (C) 2005-2011 x264 project (authors: Oskar Arvidsson, Loren Merritt, Fiona Glaser); LGPL-2.1-or-later.
- `src/h264/x86/h264_idct.asm`: `libavcodec/x86/h264_idct.asm`; Copyright (C) 2004-2005 Michael Niedermayer, Loren Merritt, Copyright (C) 2003-2008 x264 project (authors: Laurent Aimar, Loren Merritt, Holger Lubitz, Min Chen); LGPL-2.1-or-later.
- `src/h264/x86/h264_idct_10bit.asm`: `libavcodec/x86/h264_idct_10bit.asm`; Copyright (C) 2005-2011 x264 project (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264_intrapred.asm`: `libavcodec/x86/h264_intrapred.asm`; Copyright (c) 2010 Fiona Glaser, Copyright (c) 2010 Holger Lubitz, Copyright (c) 2010 Loren Merritt, Copyright (c) 2010 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/x86/h264_intrapred_10bit.asm`: `libavcodec/x86/h264_intrapred_10bit.asm`; Copyright (C) 2005-2011 x264 project (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264_intrapred_init.c`: `libavcodec/x86/h264_intrapred_init.c`; Copyright (c) 2010 Fiona Glaser; LGPL-2.1-or-later.
- `src/h264/x86/h264_qpel.c`: `libavcodec/x86/h264_qpel.c`; Copyright (c) 2004-2005 Michael Niedermayer, Loren Merritt, Copyright (c) 2011 Daniel Kang; LGPL-2.1-or-later.
- `src/h264/x86/h264_qpel_10bit.asm`: `libavcodec/x86/h264_qpel_10bit.asm`; Copyright (C) 2011 x264 project (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264_qpel_8bit.asm`: `libavcodec/x86/h264_qpel_8bit.asm`; Copyright (c) 2004-2005 Michael Niedermayer, Loren Merritt, Copyright (C) 2012 Daniel Kang (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264_weight.asm`: `libavcodec/x86/h264_weight.asm`; Copyright (c) 2004-2005 Michael Niedermayer, Loren Merritt, Copyright (C) 2010 Eli Friedman; LGPL-2.1-or-later.
- `src/h264/x86/h264_weight_10bit.asm`: `libavcodec/x86/h264_weight_10bit.asm`; Copyright (C) 2005-2011 x264 project (authors: Daniel Kang); LGPL-2.1-or-later.
- `src/h264/x86/h264chroma_init.c`: `libavcodec/x86/h264chroma_init.c`; the FFmpeg project (`h264chroma_init.c` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/h264dsp_init.c`: `libavcodec/x86/h264dsp_init.c`; Copyright (c) 2004-2005 Michael Niedermayer, Loren Merritt; LGPL-2.1-or-later.
- `src/h264/x86/mathops.h`: `libavcodec/x86/mathops.h`; Copyright (c) 2006 Michael Niedermayer et al; LGPL-2.1-or-later.
- `src/h264/x86/qpel.asm`: `libavcodec/x86/qpel.asm`; Copyright (c) 2008 Loren Merritt, Copyright (c) 2003-2013 Michael Niedermayer, Copyright (c) 2013 Daniel Kang; LGPL-2.1-or-later.
- `src/h264/x86/qpel.h`: `libavcodec/x86/qpel.h`; the FFmpeg project (`qpel.h` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/videodsp.asm`: `libavcodec/x86/videodsp.asm`; Copyright (c) 2012 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/x86/videodsp.h`: `libavcodec/x86/videodsp.h`; the FFmpeg project (`videodsp.h` names no holder); LGPL-2.1-or-later.
- `src/h264/x86/videodsp_init.c`: `libavcodec/x86/videodsp_init.c`; Copyright (C) 2002-2012 Michael Niedermayer, Copyright (C) 2012 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/h264/x86/x86inc.asm`: `libavutil/x86/x86inc.asm`; Copyright (C) 2005-2024 x264 project (authors: Loren Merritt, Henrik Gramner, Anton Mitrofanov, Fiona Glaser); ISC: permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies; THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
- `src/h264/x86/x86util.asm`: `libavutil/x86/x86util.asm`; Copyright (C) 2008-2010 x264 project (authors: Loren Merritt, Holger Lubitz); LGPL-2.1-or-later.
- `src/h264/x86/config.asm`: the settings FFmpeg's configure writes to `config.asm` for x86-64; the FFmpeg project; LGPL-2.1-or-later.
- `src/h264/aarch64/asm.S`: `libavutil/aarch64/asm.S`; Copyright (c) 2008 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/cabac.h`: `libavcodec/aarch64/cabac.h`; the FFmpeg project (`cabac.h` names no holder); LGPL-2.1-or-later.
- `src/h264/aarch64/cpu.h`: `libavutil/aarch64/cpu.h`; the FFmpeg project (`cpu.h` names no holder); LGPL-2.1-or-later.
- `src/h264/aarch64/h264chroma_init_aarch64.c`: `libavcodec/aarch64/h264chroma_init_aarch64.c`; Copyright (c) 2008 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/h264cmc_neon.S`: `libavcodec/aarch64/h264cmc_neon.S`; Copyright (c) 2008 Mans Rullgard, Copyright (c) 2013 Janne Grunau; LGPL-2.1-or-later.
- `src/h264/aarch64/h264dsp_init_aarch64.c`: `libavcodec/aarch64/h264dsp_init_aarch64.c`; Copyright (c) 2010 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/h264dsp_neon.S`: `libavcodec/aarch64/h264dsp_neon.S`; Copyright (c) 2008 Mans Rullgard, Copyright (c) 2013 Janne Grunau, Copyright (c) 2014 Janne Grunau; LGPL-2.1-or-later.
- `src/h264/aarch64/h264idct_neon.S`: `libavcodec/aarch64/h264idct_neon.S`; Copyright (c) 2008 Mans Rullgard, Copyright (c) 2013 Janne Grunau; LGPL-2.1-or-later.
- `src/h264/aarch64/h264pred_init.c`: `libavcodec/aarch64/h264pred_init.c`; Copyright (c) 2009 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/h264pred_neon.S`: `libavcodec/aarch64/h264pred_neon.S`; Copyright (c) 2009 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/h264qpel_init_aarch64.c`: `libavcodec/aarch64/h264qpel_init_aarch64.c`; Copyright (c) 2008 Mans Rullgard; LGPL-2.1-or-later.
- `src/h264/aarch64/h264qpel_neon.S`: `libavcodec/aarch64/h264qpel_neon.S`; Copyright (c) 2008 Mans Rullgard, Copyright (c) 2013 Janne Grunau; LGPL-2.1-or-later.
- `src/h264/aarch64/hpeldsp_neon.S`: `libavcodec/aarch64/hpeldsp_neon.S`; Copyright (c) 2008 Mans Rullgard, Copyright (c) 2013 Janne Grunau; LGPL-2.1-or-later.
- `src/h264/aarch64/neon.S`: `libavcodec/aarch64/neon.S`; Copyright (c) 2023 J. Dekker; LGPL-2.1-or-later.
- `src/h264/aarch64/videodsp.S`: `libavcodec/aarch64/videodsp.S`; the FFmpeg project (`videodsp.S` names no holder); LGPL-2.1-or-later.
- `src/h264/aarch64/videodsp_init.c`: `libavcodec/aarch64/videodsp_init.c`; the FFmpeg project (`videodsp_init.c` names no holder); LGPL-2.1-or-later.
- `src/h264/aarch64/config.h`: the settings FFmpeg's configure writes to `config.h` for the AArch64 assembler; the FFmpeg project; LGPL-2.1-or-later.
- `src/h264/h274.c`, `src/h264/h274.h`: `libavcodec/h274.c`, `libavcodec/h274.h`, `libavutil/film_grain_params.h`, the film grain part of `libavcodec/h2645_sei.c`; Copyright (c) 2021 Niklas Haas (`h274.c`; the tables from SMPTE RDD 5-2006); LGPL-2.1-or-later.
- `src/prores_decode.cpp`: `libavcodec/proresdec.c`, `libavcodec/proresdec.h`, `libavcodec/proresdsp.c`, `libavcodec/proresdata.c`, `libavcodec/simple_idct_template.c`, `libavcodec/x86/proresdsp.asm`, `libavcodec/x86/simple_idct10_template.asm`; Copyright (c) 2010-2011 Maxim Poliakovski, Copyright (c) 2010-2011 Elvis Presley, Copyright (c) 2001 Michael Niedermayer, Copyright (c) 2011 Ronald S. Bultje; LGPL-2.1-or-later.
- `src/dv_decode.cpp`: `libavcodec/dvdec.c`, `libavcodec/dv.c`, `libavcodec/dv_internal.h`, `libavcodec/dvdata.c`, `libavcodec/dv_profile.c`, `libavcodec/simple_idct.c`, `libavcodec/simple_idct_template.c`, `libavcodec/mathtables.c`, `libavcodec/x86/simple_idct10.asm`, `libavcodec/x86/simple_idct10_template.asm`; Copyright (c) 2002 Fabrice Bellard, Copyright (c) 2003-2004 Roman Shaposhnik, Copyright (c) 2006 Daniel Maas, Copyright (c) 2001 Michael Niedermayer, Copyright (c) 2002-2004 Michael Niedermayer, Copyright (c) 2011 Ronald S. Bultje, Copyright (c) 2015 Christophe Gisquet (`dv_profile.c` names no holder); LGPL-2.1-or-later.
- `src/h263_decode.cpp`: libavcodec/h263dec.c, ituh263dec.c, h263.c, h263.h, h263data.c, mpegvideo_motion.c, mpegvideo_dec.c, mpegvideo_unquantize.c, x86/hpeldsp.asm; Copyright (c) 2000, 2001 Fabrice Bellard; Copyright (c) 2001 Juan J. Sierralta P; Copyright (c) 2002-2004 Michael Niedermayer; Copyright (c) Nick Kurshev; Copyright (c) 2002 Zdenek Kabelac; Copyright (c) 2013 Daniel Kang; the FFmpeg project (h263data.c names no holder). LGPL-2.1-or-later.
- `src/linux/vaapi_h264.cpp`: `libavcodec/vaapi_h264.c`, `libavcodec/h264_ps.c`, `libavcodec/h264_slice.c`, `libavcodec/h264_refs.c` (parameter sets, slice headers, reference marking and lists, the stand-ins for missing references); Copyright (C) 2008-2009 Splitted-Desktop Systems, Copyright (c) 2003 Michael Niedermayer; LGPL-2.1-or-later.
- `src/linux/vaapi_hevc.cpp`: `libavcodec/vaapi_hevc.c`, `libavcodec/hevc/ps.c`, `libavcodec/hevc/refs.c`, `libavcodec/hevc/hevcdec.c`; Copyright (C) 2015 Timo Rothenpieler, Copyright (C) 2012 - 2013 Guillaume Martres, Mickael Raulet, Gildas Cocherel, Wassim Hamidouche, Copyright (C) 2013 Vittorio Giovara; LGPL-2.1-or-later.
- `src/linux/vaapi_vp9.cpp`: `libavcodec/vaapi_vp9.c`, `libavcodec/vp9.c` (frame header); Copyright (C) 2015 Timo Rothenpieler, Copyright (C) 2013 Ronald S. Bultje, Clément Bœsch; LGPL-2.1-or-later.
- `src/linux/vaapi_av1.cpp`: `libavcodec/vaapi_av1.c`, `libavcodec/cbs_av1.c`, `libavcodec/cbs_av1_syntax_template.c`, `libavcodec/av1dec.c`; the FFmpeg project (these files name no holder); LGPL-2.1-or-later.
- `src/raw_video.cpp`: `libavcodec/rawdec.c`, `libavcodec/raw.c`, `libavcodec/raw_pix_fmt_tags.h`, `libavformat/avidec.c` (pixel layouts by fourcc and bit count, row padding, bottom-up pictures, the palette at the end of the format block), `libavcodec/v210dec.c` (the line stride rules of v210); Copyright (c) 2001 Fabrice Bellard (`v210dec.c`: Copyright (C) 2009 Michael Niedermayer, Copyright (c) 2009 Baptiste Coudurier); LGPL-2.1-or-later.
- `src/qtrle_decode.cpp`: `libavcodec/qtrle.c`; Copyright (C) 2004 The FFmpeg project; LGPL-2.1-or-later.
- `src/dnxhd_decode.cpp`: `libavcodec/dnxhddec.c`, `libavcodec/simple_idct_template.c`; Copyright (c) 2007 SmartJog S.A., Baptiste Coudurier, Copyright (c) 2011 MirriAd Ltd, Copyright (c) 2015 Christophe Gisquet, Copyright (c) 2001 Michael Niedermayer; LGPL-2.1-or-later.
- `src/dnxhd_tables.h`, `src/dnxhd_tables.cpp`: `libavcodec/dnxhddata.c`, `libavcodec/dnxhddata.h` (converted by script); Copyright (c) 2007 SmartJog S.A., Baptiste Coudurier; LGPL-2.1-or-later.
- `src/hap_decode.cpp`: `libavcodec/hapdec.c`, `libavcodec/hap.c`, `libavcodec/hap.h` (LGPL-2.1-or-later; Copyright (C) 2015 Vittorio Giovara, Copyright (C) 2015 Tom Butterworth) and `libavcodec/texturedsp.c`, `libavcodec/texturedsp_template.c` (the DXT1, DXT5, YCoCg and RGTC1 block formulas; texturedsp.c is under the MIT licence below; Copyright (C) 2009 Benjamin Dobell, Glass Echidna, Copyright (C) 2012 Matthäus G. "Anteru" Chajdas, Copyright (C) 2015 Vittorio Giovara).
- `src/utvideo_decode.cpp`: `libavcodec/utvideodec.c`, `libavcodec/utvideodsp.c`, `libavcodec/lossless_videodsp.c`; Copyright (c) 2011 Konstantin Shishkov (`utvideodsp.c` and `lossless_videodsp.c` name no holder); LGPL-2.1-or-later.
- `src/ffv1_decode.cpp`: `libavcodec/ffv1dec.c`, `libavcodec/ffv1dec_template.c`, `libavcodec/ffv1.c`, `libavcodec/ffv1.h`, `libavcodec/ffv1_parse.c`, `libavcodec/ffv1_template.c`, `libavcodec/rangecoder.c`, `libavcodec/rangecoder.h`, `libavcodec/golomb.h`, `libavcodec/mathtables.c` (`ff_log2_run`); Copyright (c) 2003-2016 Michael Niedermayer, Copyright (c) 2004 Michael Niedermayer, Copyright (c) 2004 Alex Beregszaszi, Copyright (c) 2002-2004 Michael Niedermayer (`ffv1_parse.c` names no holder); LGPL-2.1-or-later.
- `src/resample.cpp`: `libswresample/rematrix.c` (the downmix matrix and its centre, surround and LFE levels), `libavfilter/af_aresample.c` (which levels a stream's downmix information chooses), `libavutil/channel_layout.c` (the layouts guessed from a channel count, in `src/audio_decode.cpp` too); Copyright (C) 2011-2012 Michael Niedermayer, Copyright (c) 2011 Stefano Sabatini, Copyright (c) 2011 Mina Nagy Zaki, Copyright (c) 2006 Michael Niedermayer; LGPL-2.1-or-later.
- `src/dts_decode.cpp`: `libavcodec/dca.c`, `libavcodec/dca_core.c`, `libavcodec/dca_core.h`, `libavcodec/dcadec.c`, `libavcodec/dcadsp.c`, `libavcodec/synth_filter.c`, `libavcodec/dcamath.h`, `libavcodec/dcaadpcm.h`, `libavcodec/dcahuff.c`, `libavcodec/dca_sample_rate_tab.h`; Copyright (C) 2016 foo86, Copyright (C) 2004 Gildas Bazin, Copyright (C) 2004 Benjamin Zores, Copyright (C) 2006 Benjamin Larsson, Copyright (C) 2007 Konstantin Shishkov, Copyright (c) 2008 Michael Niedermayer, Copyright (C) 2017 Daniil Cherednik; LGPL-2.1-or-later.
- `src/dts_tables.h`, `src/dts_tables.cpp`: `libavcodec/dcadata.c`, `libavcodec/dcahuff.c` (converted by script); Copyright (C) 2004 Gildas Bazin, Copyright (c) 2006 Benjamin Larsson, Copyright (C) 2007 Konstantin Shishkov; LGPL-2.1-or-later.
- `src/truehd_decode.cpp`: `libavcodec/mlpdec.c`, `libavcodec/mlp.c`, `libavcodec/mlp.h`, `libavcodec/mlp_parse.c`, `libavcodec/mlp_parse.h`, `libavcodec/mlpdsp.c`; Copyright (c) 2007-2008 Ian Caulfield; LGPL-2.1-or-later.
- `src/wavpack_decode.cpp`: `libavcodec/wavpack.c`, `libavcodec/wavpack.h`, `libavcodec/wavpackdata.c`, `libavformat/matroskadec.c` (the block layout of WavPack in Matroska); Copyright (c) 2006,2011 Konstantin Shishkov, Copyright (c) 2020 David Bryant, Copyright (c) 2003-2008 The FFmpeg Project; LGPL-2.1-or-later.
- `src/avi_read.cpp`: `libavformat/avidec.c` (index bases, key frames from the index flags and when an index marks none, the frame period and 25 fps for a stream header without a rate, the chunk search that skips JUNK, ix## and stray bytes), `libavformat/riff.c` (the video fourccs of each codec and the WAVE tags of AAC), `libavformat/isom_tags.c` (the HEVC fourccs), `libavformat/dv.c` and `libavcodec/dv_profile.c` (the sound of DV type-1 frames: pack places, sample counts, shuffle tables, 12-bit widening); Copyright (c) 2001 Fabrice Bellard (`riff.c`: Copyright (c) 2000 Fabrice Bellard; `dv.c`: Copyright (c) 2003 Roman Shaposhnik, Copyright (c) 2002 Fabrice Bellard, Copyright (c) 2006 Daniel Maas; `isom_tags.c`: Copyright (c) 2002 Francois Revol, Copyright (c) 2006 Baptiste Coudurier; `dv_profile.c` names no holder); LGPL-2.1-or-later.
- `src/cinepak_decode.cpp`: `libavcodec/cinepak.c`; Copyright (C) 2003 The FFmpeg project, Cinepak colorspace support (c) 2013 Rl, Aetey Global Technologies AB; LGPL-2.1-or-later.

The Bessel function of the KBD window in `src/mdct.cpp` (by way of `libavutil/mathematics.c`) comes from the Boost project's
`bessel_i0` (Copyright (c) 2006 Xiaogang Zhang), under this licence:

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization obtaining a copy of the software and accompanying
documentation covered by this license (the "Software") to use, reproduce, display, distribute, execute, and transmit the Software, and
to prepare derivative works of the Software, and to permit third-parties to whom the Software is furnished to do so, all subject to the
following:

The copyright notices in the Software and this entire statement, including the above license grant, this restriction and the following
disclaimer, must be included in all copies of the Software, in whole or in part, and all derivative works of the Software, unless such
copies or derivative works are solely in the form of machine-executable object code generated by a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR ANYONE
DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The DXT block formulas in `src/hap_decode.cpp` (by way of `libavcodec/texturedsp.c`) are under this licence:

Copyright (C) 2009 Benjamin Dobell, Glass Echidna
Copyright (C) 2012 Matthäus G. "Anteru" Chajdas (http://anteru.net)
Copyright (C) 2015 Vittorio Giovara

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to
the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
