package me.magnum.melonds

import android.util.Log

object NativeCoreLoader {
    @Volatile
    private var loaded = false

    fun load() {
        if (loaded) {
            return
        }

        synchronized(this) {
            if (loaded) {
                return
            }

            val library = "melonDS-android-frontend"
            System.loadLibrary(library)
            loaded = true

            Log.w(
                TAG,
                "backend=source_multi_profile library=$library source=HEAD",
            )
        }
    }

    private const val TAG = "NativeCore"
}
