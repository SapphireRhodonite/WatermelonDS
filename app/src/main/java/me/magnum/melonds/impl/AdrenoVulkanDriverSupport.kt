package me.magnum.melonds.impl

import android.content.Context
import android.os.Build
import android.os.Process
import me.magnum.melonds.R
import java.io.File

object AdrenoVulkanDriverSupport {
    private val gpuModel: String? by lazy {
        runCatching {
            File("/sys/class/kgsl/kgsl-3d0/gpu_model").bufferedReader().use { it.readLine() }
        }.getOrNull()
    }

    fun supportsCustomDrivers(context: Context): Boolean {
        return isLoaderAvailable(
            context.resources.getBoolean(R.bool.adrenotools_enabled),
            Build.VERSION.SDK_INT,
            Build.SUPPORTED_64_BIT_ABIS,
            Process.is64Bit(),
        )
    }

    fun supportsFreedreno(context: Context): Boolean = supportsCustomDrivers(context) && isAdrenoGpu(gpuModel)

    internal fun isAdrenoGpu(gpuModel: String?): Boolean {
        return gpuModel?.trim()?.startsWith("Adreno", ignoreCase = true) == true
    }

    internal fun isLoaderAvailable(
        buildEnabled: Boolean,
        sdkInt: Int,
        supported64BitAbis: Array<String>,
        is64BitProcess: Boolean,
    ): Boolean {
        return buildEnabled && is64BitProcess &&
            sdkInt >= Build.VERSION_CODES.P &&
            supported64BitAbis.any { it.equals("arm64-v8a", ignoreCase = true) }
    }
}
