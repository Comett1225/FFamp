package app.absentmotion.export

import android.content.Desk
import app.absentmotion.audio.OfflineMix
import app.absentmotion.audio.Resampler
import app.absentmotion.engine.EngineThread
import app.absentmotion.library.Importer
import app.absentmotion.library.ProjectStore
import app.absentmotion.model.Scene
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.ArrayBlockingQueue
import kotlin.concurrent.thread

class FmpegCompareHarness {
    private fun env(name: String): String? = System.getenv(name)?.takeIf { it.isNotBlank() }

    private fun ms(ns: Long) = "%.0f".format(ns / 1e6)

    private class Stages(val pcm: Long, val render: Long, val waiting: Long, val finish: Long, val total: Long, val written: Int)

    private suspend fun pipeline(engine: EngineThread, scene: Scene, s: ExportSettings, work: File, tag: String, dump: File?, command: ((File, File) -> List<String>)?): Stages {
        val (width, height) = exportSize(scene, s.shortSide, even = true)
        val frames = frameCount(scene, s.fps)
        val xml = scene.toXml()
        val pcm = work.resolve("$tag.pcm")
        val result = work.resolve("$tag.mp4")
        val started = System.nanoTime()
        val mix = if (command != null) ExportAudio.mixFor?.invoke(scene, s.audio.effectiveRate) else null
        if (mix != null) writePcm(mix, (scene.totalTime + 1) * 1000L, s.audio.effectiveRate, pcm) {}
        (mix as? java.io.Closeable)?.close()
        val mixed = System.nanoTime()
        val process = command?.let { ProcessBuilder(it(pcm, result)).redirectError(work.resolve("$tag.log")).redirectOutput(ProcessBuilder.Redirect.DISCARD).start() }
        val sink = process?.outputStream ?: dump?.outputStream()?.buffered(1 shl 22)
        val hashes = if (dump == null && command == null) work.resolve("$tag.hashes").printWriter() else null
        val digest = MessageDigest.getInstance("SHA-1")
        val buffers = frameBufferCount(width, height)
        val queue = ArrayBlockingQueue<ByteArray>(buffers)
        val free = ArrayBlockingQueue<ByteArray>(buffers)
        val end = ByteArray(0)
        var written = 0
        var failure: Throwable? = null
        val writer = thread(name = "harness-write") {
            try {
                while (true) {
                    val pixels = queue.take()
                    if (pixels === end) break
                    sink?.write(pixels)
                    hashes?.println(digest.digest(pixels).joinToString("") { "%02x".format(it) })
                    free.put(pixels)
                    written++
                }
                sink?.flush()
            } catch (t: Throwable) {
                failure = t
                while (true) { val p = queue.take(); if (p === end) break; free.offer(p) }
            }
        }
        repeat(buffers) { free.add(ByteArray(width * height * 4)) }
        var render = 0L
        var waiting = 0L
        engine.exportSession(xml, s.smoothing, s.fps) {
            try {
                for (k in 0 until frames) {
                    var t = System.nanoTime()
                    val pixels = free.take()
                    waiting += System.nanoTime() - t
                    t = System.nanoTime()
                    val got = frameInto(frameTimeMs(scene, s.fps, k), width, height, pixels)
                    render += System.nanoTime() - t
                    when (got) {
                        1 -> queue.put(pixels)
                        0 -> free.put(pixels)
                        else -> error("frame $k was not drawn")
                    }
                }
                var t = System.nanoTime()
                val pixels = free.take()
                waiting += System.nanoTime() - t
                t = System.nanoTime()
                check(lastFrameInto(pixels) == 1) { "last frame was not drawn" }
                render += System.nanoTime() - t
                queue.put(pixels)
            } finally {
                lastFrameInto(null)
            }
        }
        queue.put(end)
        writer.join()
        val drawn = System.nanoTime()
        sink?.close()
        hashes?.close()
        val code = process?.waitFor() ?: 0
        val done = System.nanoTime()
        check(failure == null) { "writer failed: $failure ${work.resolve("$tag.log").takeIf { it.exists() }?.readText()}" }
        check(code == 0) { "encoder exit $code: ${work.resolve("$tag.log").readText()}" }
        if (env("FM_KEEP") == null) pcm.delete()
        return Stages(mixed - started, render, waiting, done - drawn, done - started, written)
    }

    @Test fun run() = runBlocking {
        val mode = env("FM_MODE")
        assumeTrue("set FM_MODE to run", mode != null)
        val work = File(env("FM_WORK")!!).also { it.mkdirs() }
        env("FM_NATIVE")?.let { System.setProperty("am.native", it) }
        env("FM_FFMPEG")?.let { System.setProperty("am.ffmpeg", it) }
        env("FM_FFPROBE")?.let { System.setProperty("am.ffprobe", it) }
        val tag = env("FM_TAG") ?: mode!!
        System.setProperty("am.exports", work.resolve("home-$tag").path)
        System.setProperty("am.data", work.resolve("data-$tag").path)
        val runs = env("FM_RUNS")?.toInt() ?: 1
        val out = work.resolve("result-$tag.txt")
        out.writeText("")
        fun line(text: String) { out.appendText(text + "\n"); println(text) }

        val store = ProjectStore(Desk.context)
        val reports = Importer.forApp(Desk.context, store).import { File(env("FM_PROJECT")!!).inputStream() }
        val report = reports.first()
        line("imported '${report.title}' as ${report.id}")
        for ((head, items) in report.sections) line("import note: ${head.resolve()}: ${items.joinToString("; ") { it.resolve() }}")
        val scene = checkNotNull(store.load(report.id))
        val s = ExportSettings(minOf(scene.width, scene.height), scene.fps)
        val (width, height) = exportSize(scene, s.shortSide, even = true)
        val frames = frameCount(scene, s.fps)
        val bitrate = videoBitrate(width, height, s)
        line("scene ${scene.width}x${scene.height} ${scene.fps} fps totalTime ${scene.totalTime} ms; export ${width}x$height, $frames frames, video $bitrate bit/s ${s.codec}, audio ${s.audio.codec} ${s.audio.bitrateKbps} kbit/s ${s.audio.effectiveRate} Hz ${s.audio.channels} ch")
        line("native library ${System.getProperty("am.native")}")
        line("ffmpeg ${app.absentmotion.desk.DeskMedia.ffmpeg}")

        ExportAudio.mixFor = { sc, rate -> OfflineMix.of(Desk.context, sc, Resampler.ULTRA, rate) }
        val engine = EngineThread(0xff000000.toInt(), Desk.context)
        try {
            engine.effects.first { it.definitions.isNotEmpty() }
            val amedia = env("FM_AMEDIA")
            val absent = { pcm: File, result: File ->
                listOf(amedia!!, "encode", result.path, "--video", "-", "--format", "rgb0", "--size", "${width}x$height", "--fps", "${s.fps}", "--codec", "h264",
                    "--bitrate", "$bitrate", "--gop", "${s.fps}") + (if (env("FM_REORDER") == null) listOf("--no-reorder") else emptyList()) +
                    listOf("--audio", pcm.path, "--rate", "${s.audio.effectiveRate}", "--channels", "2", "--abitrate", "${s.audio.bitrateKbps * 1000}")
            }
            val today = { pcm: File, result: File ->
                val encoder = DeskEncoders.video(s.codec).first { DeskEncoders.works(it, s, width, height) }
                val coder = DeskEncoders.audio(s.audio.codec, s.audio.bitrateKbps).first()
                listOf(app.absentmotion.desk.DeskMedia.ffmpeg, "-hide_banner", "-v", "error") + DeskEncoders.device(encoder) +
                    listOf("-f", "rawvideo", "-pix_fmt", "rgb0", "-s", "${width}x$height", "-r", "${s.fps}", "-i", "-", "-f", "s16le", "-ar", "${s.audio.effectiveRate}", "-ac", "2", "-i", pcm.path) +
                    listOf("-map", "0:v") + DeskEncoders.filter(encoder) + DeskEncoders.videoArgs(encoder, s, width, height) +
                    listOf("-map", "1:a", "-c:a", coder) + DeskEncoders.audioExtra(coder) + listOf("-b:a", "${s.audio.bitrateKbps}k", "-ar", "${s.audio.effectiveRate}", "-ac", "${s.audio.channels}") +
                    listOf("-f", "mp4", "-movflags", "+faststart", "-y", result.path)
            }
            for (k in 0 until runs) {
                val name = "$tag-$k"
                when (mode) {
                    "app" -> {
                        val exporter = Exporter(Desk.context, engine, MediaStoreExport(Desk.context))
                        val t = System.nanoTime()
                        val notes = ArrayList<String>()
                        val result = exporter.export(ExportKind.Video, scene, s, 0, name, note = { notes += it.resolve() }) {}
                        val total = System.nanoTime() - t
                        val file = result.uris.first().toFile()!!
                        file.copyTo(work.resolve("$name.mp4"), overwrite = true)
                        file.delete()
                        line("run $k app-export total_ms ${ms(total)} encoder ${EncoderTrouble.last} notes $notes")
                    }
                    "stages-ffmpeg", "stages-absent" -> {
                        val st = pipeline(engine, scene, s, work, name, null, if (mode == "stages-ffmpeg") today else absent)
                        line("run $k $mode pcm_ms ${ms(st.pcm)} render_ms ${ms(st.render)} waiting_ms ${ms(st.waiting)} finish_ms ${ms(st.finish)} total_ms ${ms(st.total)} frames ${st.written}")
                    }
                    "render" -> {
                        val st = pipeline(engine, scene, s, work, name, null, null)
                        line("run $k render render_ms ${ms(st.render)} total_ms ${ms(st.total)} frames ${st.written}")
                    }
                    "dump" -> {
                        val st = pipeline(engine, scene, s, work, name, File(env("FM_DUMP")!!), null)
                        line("run $k dump render_ms ${ms(st.render)} total_ms ${ms(st.total)} frames ${st.written}")
                    }
                    else -> error("unknown mode $mode")
                }
            }
        } finally {
            engine.close()
        }
    }
}
