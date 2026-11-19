package me.magnum.melonds.common.cheats

import java.io.FilterInputStream
import java.io.InputStream

class ProgressTrackerInputStream(inputStream: InputStream?) : FilterInputStream(inputStream) {
    var totalReadBytes = 0L
        private set

    override fun read(): Int {
        val value = super.read()
        if (value >= 0) totalReadBytes++
        return value
    }

    override fun skip(n: Long): Long = super.skip(n).also { totalReadBytes += it }

    override fun read(b: ByteArray?, off: Int, len: Int): Int {
        val readBytes = super.read(b, off, len)
        if (readBytes > 0) totalReadBytes += readBytes

        return readBytes
    }
}