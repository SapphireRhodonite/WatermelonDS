package me.magnum.melonds.multiplayer.phantom

import android.content.Context
import android.net.Uri
import dagger.hilt.android.qualifiers.ApplicationContext
import me.magnum.melonds.R
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.model.rom.config.RomGbaSlotConfig
import me.magnum.melonds.domain.model.rom.config.RuntimeConsoleType
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.impl.RomSaveFileManager
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.UUID
import javax.inject.Inject

class PhantomSaves @Inject constructor(
    @ApplicationContext private val context: Context,
    private val saves: RomSaveFileManager,
    private val settings: SettingsRepository,
) {
    private val root get() = File(context.noBackupFilesDir, "phantom")

    fun pending(rom: Rom): File? = root.listFiles()?.singleOrNull {
        val origin = File(it, "rom")
        origin.isFile && origin.readText() == rom.uri.toString()
    }

    suspend fun prepare(rom: Rom): File {
        val config = settings.getEmulatorConfiguration(rom.config, rom.resolvedDsiProfile)
        require(!rom.isDsiWareTitle && rom.config.runtimeConsoleType != RuntimeConsoleType.DSi &&
            config.consoleType == ConsoleType.DS && !config.dldiSdCardConfiguration.enabled &&
            rom.config.gbaSlotConfig !is RomGbaSlotConfig.GbaRom) { context.getString(R.string.phantom_ds_only) }
        val source = saves.findExistingSaveFile(rom)?.uri
            ?: error(context.getString(R.string.rom_save_file_missing))
        require(saves.isPlausibleSaveFile(source)) { context.getString(R.string.rom_save_file_missing) }
        val directory = File(root, UUID.randomUUID().toString())
        check(directory.mkdirs())
        try {
            val backup = File(directory, "original.sav")
            saves.openInputStream(source).use { input ->
                backup.outputStream().use { out -> input.copyTo(out); out.fd.sync() }
            }
            check(backup.length() > 0 && matches(source, backup)) { context.getString(R.string.phantom_conflict) }
            File(directory, "source").writeText(source.toString())
            for (role in listOf("primary", "peer")) {
                val player = File(directory, role)
                check(player.mkdir())
                backup.copyTo(File(player, "game.sav"))
                if (config.useCustomBios) {
                    val firmware = checkNotNull(config.dsFirmwareUri)
                    saves.openInputStream(firmware).use { input ->
                        File(player, "firmware.bin").outputStream().use { input.copyTo(it) }
                    }
                }
                val localWfc = File(config.internalDirectory, "wfcsettings.bin")
                val wfc = config.wfcSettingsPath?.let(Uri::parse) ?: localWfc.takeIf { it.isFile }?.let(Uri::fromFile)
                if (wfc != null) saves.openInputStream(wfc).use { input ->
                    File(player, "wfcsettings.bin").outputStream().use { input.copyTo(it) }
                }
            }
            File(directory, "rom").writeText(rom.uri.toString())
            return directory
        } catch (e: Throwable) {
            directory.deleteRecursively()
            throw e
        }
    }

    fun finish(directory: File, keep: Boolean) {
        RandomAccessFile(File(directory, "primary/owner"), "rw").use { primary ->
            primary.channel.tryLock().use { mainLock ->
                check(mainLock != null) { context.getString(R.string.phantom_busy) }
                RandomAccessFile(File(directory, "peer/owner"), "rw").use { peer ->
                    peer.channel.tryLock().use { peerLock ->
                        check(peerLock != null) { context.getString(R.string.phantom_busy) }
                        val source = Uri.parse(File(directory, "source").readText())
                        val original = File(directory, "original.sav")
                        val result = File(directory, "primary/game.sav")
                        val publishing = File(directory, "publishing")
                        if (keep || publishing.exists()) {
                            val target = if (keep) result else original
                            require(target.length() > 0)
                            if (!matches(source, target)) {
                                check(matches(source, original) || (publishing.exists() && wasInterrupted(source, result, original))) {
                                    context.getString(R.string.phantom_conflict)
                                }
                                publishing.outputStream().use { it.fd.sync() }
                                target.inputStream().use { input -> saves.openOutputStream(source).use { input.copyTo(it) } }
                                check(matches(source, target)) { context.getString(R.string.phantom_save_failed) }
                            }
                        }
                        check(directory.deleteRecursively())
                    }
                }
            }
        }
    }

    private fun matches(uri: Uri, file: File): Boolean {
        val saved = saves.openInputStream(uri).use(::digest)
        return file.inputStream().use(::digest).contentEquals(saved)
    }

    private fun digest(input: InputStream): ByteArray {
        val hash = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            hash.update(buffer, 0, count)
        }
        return hash.digest()
    }

    private fun wasInterrupted(source: Uri, result: File, original: File): Boolean {
        fun prefixOf(file: File): Boolean = saves.openInputStream(source).buffered().use { current ->
            file.inputStream().buffered().use { expected ->
                var next = current.read()
                while (next >= 0) {
                    if (expected.read() != next) return false
                    next = current.read()
                }
                true
            }
        }
        return prefixOf(result) || prefixOf(original)
    }
}
