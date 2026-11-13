# Notices

FFamp files whose code or tables are based on FFmpeg, one line each: the FFamp file, the FFmpeg files it is based on, and their
copyright holders. Those FFmpeg files are licensed LGPL-2.1-or-later.

- `src/wma_decode.cpp`: `libavcodec/wmadec.c`, `libavcodec/wma.c`, `libavcodec/wma.h`; Copyright (c) 2002-2007 The FFmpeg Project; LGPL-2.1-or-later.
- `src/wma_common.cpp`: `libavcodec/wma.c`, `libavcodec/wma_common.c`; Copyright (c) 2002-2007 The FFmpeg Project (`wma_common.c` names no holder); LGPL-2.1-or-later.
- `src/wma.h`: `libavcodec/wma.h`; Copyright (c) 2002-2007 The FFmpeg Project; LGPL-2.1-or-later.
- `src/wma_tables.cpp`: `libavcodec/wmadata.h`, `libavcodec/wmadec.c`, `libavcodec/wma_freqs.c`, `libavcodec/aactab.c`; Copyright (c) 2002 The FFmpeg Project, Copyright (c) 2005-2006 Oded Shimon, Copyright (c) 2006-2007 Maxim Gavrilov (`wma_freqs.c` names no holder); LGPL-2.1-or-later.
- `src/mdct.cpp`: `libavutil/tx_template.c`, `libavcodec/sinewin_tablegen.h`; Copyright (c) Lynne, Copyright (c) 2008 Loren Merritt, Copyright (c) 2002 Fabrice Bellard, Copyright (c) 2009 Reimar Döffinger; LGPL-2.1-or-later.
- `src/vlc.cpp`: `libavcodec/vlc.c`; Copyright (c) 2000, 2001 Fabrice Bellard, Copyright (c) 2002-2004 Michael Niedermayer, Copyright (c) 2010 Loren Merritt; LGPL-2.1-or-later.
- `src/wmapro_decode.cpp`: `libavcodec/wmaprodec.c`, `libavcodec/wmaprodata.h`, `libavutil/float_dsp.c`; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson (`wmaprodata.h`: Copyright (c) 2008-2009 Sascha Sommer), Copyright 2005 Balatoni Denes, Copyright 2006 Loren Merritt; LGPL-2.1-or-later.
- `tests/fixtures/wmapro/make.py`: writes the bitstream `libavcodec/wmaprodec.c` reads, with the tables of `src/wmapro_decode.cpp`; Copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion, Copyright (c) 2008-2011 Sascha Sommer, Benjamin Larsson; LGPL-2.1-or-later.
