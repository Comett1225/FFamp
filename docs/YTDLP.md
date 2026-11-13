# The video downloader without FFmpeg

`DeskVideoDownloader.kt` gives yt-dlp `--ffmpeg-location` today and lets it merge, convert and repair with FFmpeg. yt-dlp is not
changed and `amedia` never starts it. The app tells yt-dlp to download the pieces only and then makes one `amedia` call itself.

Nothing in this file was run against yt-dlp in this round (it is not installed on the build machine and the tests use no network).
The option names are yt-dlp's documented ones; the `amedia` side is tested with files of the same kinds (fragmented MP4 and WebM
pieces, a transport stream).

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
