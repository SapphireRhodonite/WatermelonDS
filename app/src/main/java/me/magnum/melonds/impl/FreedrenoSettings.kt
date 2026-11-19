package me.magnum.melonds.impl

import android.content.Context
import android.system.Os
import android.util.Log
import androidx.preference.PreferenceManager

object FreedrenoSettings {
    val variables = listOf("FD_DEV_FEATURES", "TU_DEBUG", "FD_MESA_DEBUG")

    fun preferenceKey(variable: String) = "video_freedreno_${variable.lowercase(java.util.Locale.ROOT)}"

    fun isValid(value: String): Boolean = value.length <= 2048 && value.all { it in ' '..'~' }

    fun applyAtStartup(context: Context) {
        if (!AdrenoVulkanDriverSupport.supportsFreedreno(context)) return
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        for (variable in variables) {
            val value = (preferences.all[preferenceKey(variable)] as? String).orEmpty().trim()
            if (!isValid(value)) {
                Log.w("FreedrenoSettings", "Ignoring invalid $variable")
                continue
            }
            runCatching {
                if (value.isEmpty()) Os.unsetenv(variable) else Os.setenv(variable, value, true)
                Log.i("FreedrenoSettings", "$variable=${Os.getenv(variable) ?: "<driver default>"}")
            }.onFailure {
                Log.w("FreedrenoSettings", "Could not configure $variable", it)
            }
        }
    }
}
