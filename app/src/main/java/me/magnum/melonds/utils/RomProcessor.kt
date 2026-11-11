package me.magnum.melonds.utils

import android.graphics.Bitmap
import android.graphics.Color
import androidx.core.graphics.createBitmap
import me.magnum.melonds.common.Crc32
import me.magnum.melonds.domain.model.CartridgeId
import me.magnum.melonds.domain.model.RomInfo
import me.magnum.melonds.domain.model.RomMetadata
import me.magnum.melonds.domain.model.rom.Rom
import java.io.InputStream
import java.math.BigInteger
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import kotlin.experimental.and
import kotlin.math.min

object RomProcessor {
	private val DSIWARE_CATEGORY = 0x00030004.toUInt()
	private const val MAX_ARM_BOOTCODE_SIZE = 0x3BFE00

	@Suppress("NAME_SHADOWING")
	fun getRomMetadata(inputStream: InputStream): RomMetadata? {
		val sectionReader = ForwardRomSectionReader(inputStream)
		val header = sectionReader.readSection(0, 0x160) ?: return null
		val gameCode = String(header, 0x0C, 4)

		val arm9Offset = byteArrayToInt(header, 0x20)
		val arm9Size = byteArrayToInt(header, 0x2C)
		if (arm9Size !in 0..MAX_ARM_BOOTCODE_SIZE) return null

		val arm7Offset = byteArrayToInt(header, 0x30)
		val arm7Size = byteArrayToInt(header, 0x3C)
		if (arm7Size !in 0..MAX_ARM_BOOTCODE_SIZE) return null

		val bannerOffset = byteArrayToInt(header, 0x68)

		val isDsiWareTitle = if (gameCode[0] == 'H' || gameCode[0] == 'K') {
			// This is probably a DSi Ware game. Confirm with the title category field.
			val categoryData = sectionReader.readSection(0x234, 4) ?: return null
			byteArrayToInt(categoryData).toUInt() == DSIWARE_CATEGORY
		} else {
			false
		}

		var arm9Bootcode: ByteArray? = null
		var arm7Bootcode: ByteArray? = null
		var banner: ByteArray? = null
		val requiredSections = listOf(
			RequiredRomSection(arm9Offset, arm9Size, RequiredRomSection.Type.ARM9),
			RequiredRomSection(arm7Offset, arm7Size, RequiredRomSection.Type.ARM7),
			RequiredRomSection(bannerOffset, 0xA00, RequiredRomSection.Type.BANNER),
		).sortedBy { it.offset }

		for (section in requiredSections) {
			val data = sectionReader.readSection(
				section.offset,
				section.size,
				padToSize = section.type == RequiredRomSection.Type.BANNER,
			) ?: return null
			when (section.type) {
				RequiredRomSection.Type.ARM9 -> arm9Bootcode = data
				RequiredRomSection.Type.ARM7 -> arm7Bootcode = data
				RequiredRomSection.Type.BANNER -> banner = data
			}
		}

		val arm9Data = arm9Bootcode ?: return null
		val arm7Data = arm7Bootcode ?: return null
		val bannerData = banner ?: return null

		val bannerText = readBannerTitleAndDeveloper(bannerData)
		val romName = bannerText?.first.orEmpty()
		val developerName = bannerText?.second.orEmpty()

		val retroAchievementsMd5Digest = MessageDigest.getInstance("MD5").run {
			update(header)
			update(arm9Data)
			update(arm7Data)
			update(bannerData)
			digest()
		}

		val retroAchievementsHash = BigInteger(1, retroAchievementsMd5Digest).toString(16).padStart(32, '0')

		return RomMetadata(
			romName,
			developerName,
			isDsiWareTitle,
			retroAchievementsHash,
			CartridgeId(gameCode, header[0x1E].toInt() and 0xFF),
		)
	}

	private fun readBannerTitleAndDeveloper(banner: ByteArray): Pair<String, String>? {
		val version = byteArrayToShort(banner, 0).toInt()
		if (version !in 1..3) {
			return null
		}

		val titleData = banner.copyOfRange(0x340, 0x340 + 256)
		val titleString = String(titleData, StandardCharsets.UTF_16LE).trim().replace("\u0000", "")
		if (titleString.isBlank()) {
			return null
		}

		val title = titleString.substringBeforeLast('\n').replace("\n", " ")
		val developer = titleString.substringAfterLast('\n', "")
		return title to developer
	}

	fun getRomIcon(inputStream: InputStream): Bitmap {
		// Banner offset is at header offset 0x68
		inputStream.skipStreamBytes(0x68)
		// Obtain the banner offset
		val offsetData = ByteArray(4)
		inputStream.read(offsetData)

		val bannerOffset = byteArrayToInt(offsetData)
		inputStream.skipStreamBytes(bannerOffset.toLong() + 32 - (0x68 + 4))
		val tileData = ByteArray(512)
		inputStream.read(tileData)

		val paletteData = ByteArray(16 * 2)
		inputStream.read(paletteData)

		val palette = UShortArray(16)
		for (i in 0 until 16) {
			// Each palette color is 16 bits. Join pairs of bytes to create the correct color
			val lower = paletteData[i * 2]
			val upper = paletteData[(i * 2) + 1]

			val value = ((upper.toInt() and 0xFF).shl(8) or (lower.toInt() and 0xFF)).toUShort()
			palette[i] = value
		}

		val argbPalette = paletteToArgb(palette)
		val icon = processTiles(tileData, argbPalette)
		val bitmapData = iconToBitmapArray(icon)

		val bitmap = createBitmap(32, 32)
		bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(bitmapData))
		return bitmap
	}

	fun getRomInfo(rom: Rom, inputStream: InputStream): RomInfo? {
		val romHeader = ForwardRomSectionReader(inputStream).readSection(0, 0x200) ?: return null

		val gameTitle = romHeader.decodeToString(endIndex = 12)
		val gameCode = romHeader.decodeToString(startIndex = 12, endIndex = 12 + 4)
		val headerChecksum = Crc32.compute(romHeader)
		return RomInfo(gameCode, headerChecksum, gameTitle, rom.name, romHeader[0x1E].toInt() and 0xFF)
	}

	private fun byteArrayToInt(intData: ByteArray, offset: Int = 0): Int {
		// NDS is little endian. Reorder bytes as needed
		// Also make sure that every byte is treated as an unsigned integer
		return  (intData[offset + 0].toInt() and 0xFF) or
				(intData[offset + 1].toInt() and 0xFF).shl(8) or
				(intData[offset + 2].toInt() and 0xFF).shl(16) or
				(intData[offset + 3].toInt() and 0xFF).shl(24)
	}

	private fun byteArrayToShort(shortData: ByteArray, offset: Int = 0): UShort {
		return ((shortData[offset + 0].toInt() and 0xFF) or
				(shortData[offset + 1].toInt() and 0xFF).shl(8)).toUShort()
	}

	private fun paletteToArgb(palette: UShortArray): IntArray {
		val argbPalette = IntArray(16)
		for (i in 0 until 16) {
			val color = palette[i]

			val red =   getColor(color, 0).toInt() and 0xFF
			val green = getColor(color, 5).toInt() and 0xFF
			val blue =  getColor(color, 10).toInt() and 0xFF

			val argbColor = Color.argb(if (i == 0) 0 else 255, red, green, blue)
			argbPalette[i] = argbColor
		}

		return argbPalette
	}

	private fun processTiles(tileData: ByteArray, palette: IntArray): IntArray {
		val image = IntArray(32 * 32)

		for (ty in 0 until 4) {
			for (tx in 0 until 4) {
				for (i in 0 until 32) {
					val data = tileData[(ty * 4 + tx) * 32 + i]
					val first = ((data and 0xF0.toByte()).toInt() and 0xFF).shr(4)
					val second = (data.toInt() and 0xF)

					val outputX = tx * 8 + (i % 4) * 2
					val outputY = ty * 8 + i / 4
					val finalPos = outputY * 32 + outputX

					if (second == 0)
						image[finalPos] = 0
					else
						image[finalPos] = palette[second]

					if (first == 0)
						image[finalPos + 1] = 0
					else
						image[finalPos + 1] = palette[first]
				}
			}
		}

		return image
	}

	private fun iconToBitmapArray(icon: IntArray): ByteArray {
		val bitmapArray = ByteArray(32 * 32 * 4)

		for (i in icon.indices) {
			val argbColor = icon[i]

			bitmapArray[i * 4] = (argbColor.shr(16) and 0xFF).toByte()
			bitmapArray[i * 4 + 1] = (argbColor.shr(8) and 0xFF).toByte()
			bitmapArray[i * 4 + 2] = (argbColor and 0xFF).toByte()
			bitmapArray[i * 4 + 3] = (argbColor.shr(24) and 0xFF).toByte()
		}

		return bitmapArray
	}

	private fun getColor(color: UShort, offset: Int): Byte {
		val rawColor = (getRawColor(color, offset).toInt() and 0xFF)
		return ((rawColor.shl(3) + rawColor.shr(2)) and 0xFF).toByte()
	}

	private fun getRawColor(color: UShort, offset: Int): Byte {
		// Fetch 5 bits at the given offset
		return (color.toInt().shr(offset) and 0x1F).toByte()
	}

	/**
	 * Custom made way to skip bytes in an input stream. When dealing with zipped files, the internal implementations (ZipInputStream and BufferedInputStream) don't work very
	 * well. This one seems to work when dealing with a BufferedInputStream
	 */
	private fun InputStream.skipStreamBytes(bytes: Long) {
		val buffer = ByteArray(1024)
		var remaining = bytes
		do {
			val toRead = min(remaining, buffer.size.toLong())
			val read = this.read(buffer, 0, toRead.toInt())
			if (read <= 0) {
				break
			}
			remaining -= read
		} while (remaining > 0)
	}

	private data class RequiredRomSection(
		val offset: Int,
		val size: Int,
		val type: Type,
	) {
		enum class Type {
			ARM9,
			ARM7,
			BANNER,
		}
	}

	private class ForwardRomSectionReader(private val stream: InputStream) {
		private val buffer = ByteArray(8192)
		private var position = 0L

		fun readSection(offset: Int, size: Int, padToSize: Boolean = false): ByteArray? {
			if (offset < 0 || size < 0) {
				return null
			}
			val targetOffset = offset.toLong()
			if (targetOffset < position) {
				return null
			}
			val endOffset = targetOffset + size
			if (endOffset < targetOffset) {
				return null
			}

			if (!skipTo(targetOffset)) {
				return null
			}

			val section = ByteArray(size)
			var totalRead = 0
			while (totalRead < size) {
				val read = stream.read(section, totalRead, size - totalRead)
				if (read <= 0) {
					return if (read < 0 && padToSize) section else null
				}
				totalRead += read
				position += read
			}

			return section
		}

		private fun skipTo(targetOffset: Long): Boolean {
			var remaining = targetOffset - position
			while (remaining > 0) {
				val toRead = min(buffer.size.toLong(), remaining).toInt()
				val read = stream.read(buffer, 0, toRead)
				if (read <= 0) {
					return false
				}
				position += read
				remaining -= read
			}
			return true
		}
	}
}
