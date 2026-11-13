# The video downloader without FFmpeg

`DeskVideoDownloader.kt` gives yt-dlp `--ffmpeg-location` today and lets it merge, convert and repair with FFmpeg. yt-dlp is not
changed and `amedia` never starts it. The app tells yt-dlp to download the pieces only and then makes one `amedia` call itself.

Checked for real on 2026-11-13 with yt-dlp 2026.10.26 and one Creative Commons film (Blender's "Caminandes 3: Llamigos", 2.5 minutes,
on YouTube), FFmpeg hidden with the options below; the downloads were deleted afterwards:

| Download | yt-dlp wrote | `amedia` call | Judged by |
|---|---|---|---|
| `-f 134,140` (H.264 + AAC, DASH) | `item.134.mp4`, `item.140.m4a` | `merge` gives `item.mp4`, 3601 frames | AVFoundation plays it with picture and sound, VLC opens both tracks, ffprobe counts the same packets |
| `-f 243,251` (VP9 + Opus, DASH) | `item.243.webm`, `item.251.webm` | `merge` gives `item.webm` | VLC opens both tracks, `mkvmerge -J` reports no errors or warnings |
| H.264 `.mp4` + Opus `.webm` | as above | `merge` gives an `.mp4` with Opus | AVFoundation and VLC |
| `-f 229` (HLS, picture only) | `hls.mp4`, really a transport stream | `probe --json` says `mpegts`, `rewrap` gives a plain MP4 | AVFoundation, VLC; FFmpeg decodes the same 3601 pictures from both |
| `-f 18` (one file with both) | `both.mp4` | none needed; `strip-audio` gives the silent copy | AVFoundation |
| sound only | `item.140.m4a`, `item.251.webm` | `extract-audio` gives `.m4a` and `.opus` | AVFoundation (M4A), VLC |

What reality added to this file:

- yt-dlp prints `WARNING: ffmpeg-location ... does not exist! Continuing without ffmpeg` on every run. It is expected; the app
  must not show it as an error.
- YouTube's HLS pieces pad the end of each piece with transport packets that carry no payload. The reader lost the last picture of
  every piece on them (27 of 3601 here); fixed in `src/ts_read.cpp`, with a check in the suite.
- YouTube needs yt-dlp's JavaScript helper (`deno` here). That is yt-dlp's own need and has nothing to do with FFmpeg.

## What yt-dlp is given

Always, instead of `--ffmpeg-location <ffmpeg>`:

    --ffmpeg-location <a folder that does not exist> --fixup never --downloader "m3u8:native" --downloader "dash:native"

- `--ffmpeg-location` pointing nowhere keeps yt-dlp from finding an FFmpeg that happens to be on the user's PATH, so every machine
  behaves the same.
- `--fixup never` switches off the repair steps that need FFmpeg (transport stream in an MP4 name, DASH M4A container, timestamps).
  `amedia` does the ones that matter, see below.
- The native downloaders write HLS and DASH pieces themselves; without them yt-dlp would hand such streams to FFmpeg.

| The app wants | yt-dlp options today | yt-dlp options without FFmpeg |
|---|---|---|
| video + sound from two formats | `-f V+A --merge-output-format mp4\|webm -o item.%(ext)s` | `-f V,A -o item.%(format_id)s.%(ext)s` |
| one format that has both | `-f V -o item.%(ext)s` | the same |
| sound only | `-f A -x --audio-format m4a` | `-f A -o item.%(ext)s` |
| picture only from a format that has sound | `-f V`, then FFmpeg `-map 0:v -c copy` | `-f V -o item.%(ext)s` |

`-f V,A` (a comma, not a plus) downloads both formats as two separate files and never asks for a merge; the format id in the name
keeps them apart. With `-f V+A` and no FFmpeg yt-dlp also keeps both files, but only after a warning, and it names them
`item.f<V>.<ext>` and `item.f<A>.<ext>`; the comma form is the documented way to get two files.

Progress: with two files the `[download] Destination:` lines still arrive once per file, so the app's own counting of pieces stays
as it is. The merge is a copy and takes well under a second for most files; its `progress` lines can fill the last few percent.

## What the app then calls

| Case | Call | Result |
|---|---|---|
| two pieces | `amedia merge item.<V>.<ext> item.<A>.<ext> item` | prints `output item.mp4` (or `.webm`, `.mkv`) and writes it. To force the container the app chose before (`plan.webm`), pass `item.webm` or `item.mp4`; a codec that container cannot hold is refused |
| one file, HLS download (transport stream, often under an `.mp4` name) | `amedia rewrap item.mp4 out.mp4` | MP4 with the header in front, picture and sound in step |
| one file, already MP4 or WebM | none needed. `amedia rewrap in out.mp4` turns a fragmented MP4 into a plain one with the header in front |
| sound only | `amedia extract-audio item.<ext> out` | copies the sound and prints `output out.m4a` for AAC, `out.opus` for Opus, `out.mp3` for MP3. For "always M4A" as today: `amedia extract-audio item.<ext> out.m4a --aac --bitrate 160000` re-encodes whatever it is |
| picture only, file has sound | `amedia strip-audio item.<ext> silent.<ext>` | the same container without the sound track |

Whether a downloaded file is a transport stream does not have to be guessed: `amedia probe --json item.mp4` reports
`format_name` `mpegts`; calling `rewrap` on every single-file download is also fine, it is a copy.

After a call that exits with 0 the app deletes the pieces. On any other exit code nothing was written; the pieces are still there.

## What is not covered

- **Encrypted or DRM streams** and anything else yt-dlp itself needs FFmpeg for (live streams recorded through FFmpeg, some
  `rtmp`/`mms` protocols): yt-dlp reports an error; there is no replacement.
- **Subtitles, thumbnails, chapters and metadata embedding**: the app does not ask for them today; `amedia` does not write them.
- **Codecs our writers cannot store**: AC-3 and E-AC-3 sound go into `.mkv` only if the Matroska writer knows them; MP3 sound next
  to video goes into `.mkv`, not `.mp4`.
- A transport stream whose sound and picture start more than a few seconds apart is copied as it is, with the gap.
