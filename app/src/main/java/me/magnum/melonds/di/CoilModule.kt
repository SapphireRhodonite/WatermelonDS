package me.magnum.melonds.di

import android.content.Context
import coil.ImageLoader
import coil.intercept.Interceptor
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import me.magnum.melonds.impl.BackgroundThumbnailProvider
import me.magnum.melonds.impl.RomIconProvider
import me.magnum.melonds.impl.image.CoilBackgroundThumbnailFetcher
import me.magnum.melonds.impl.image.CoilRomIconFetcher
import me.magnum.melonds.impl.image.CoilURLMapper
import me.magnum.melonds.ui.romlist.boxart.BoxArtRepository
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object CoilModule {

    @Provides
    fun provideBackgroundThumbnailFetcher(backgroundThumbnailProvider: BackgroundThumbnailProvider): CoilBackgroundThumbnailFetcher.Factory {
        return CoilBackgroundThumbnailFetcher.Factory(backgroundThumbnailProvider)
    }

    @Provides
    fun provideRomIconFetcher(romIconProvider: RomIconProvider): CoilRomIconFetcher.Factory {
        return CoilRomIconFetcher.Factory(romIconProvider)
    }

    @Provides
    @Singleton
    fun provideCoilImageLoader(
        @ApplicationContext context: Context,
        coilBackgroundThumbnailFetcherFactory: CoilBackgroundThumbnailFetcher.Factory,
        coilRomIconFetcherFactory: CoilRomIconFetcher.Factory,
        boxArtRepository: BoxArtRepository,
    ): ImageLoader {
        val coverLocks = Array(16) { Mutex() }
        return ImageLoader.Builder(context)
            .components {
                add(Interceptor { chain ->
                    val request = chain.request
                    val key = (request.data as? String)?.let(boxArtRepository::imageCacheKey)
                    if (key == null) return@Interceptor chain.proceed(request)

                    val coverRequest = request.newBuilder().diskCacheKey(key).build()
                    coverLocks[Math.floorMod(key.hashCode(), coverLocks.size)].withLock {
                        chain.proceed(coverRequest)
                    }
                })
                add(CoilURLMapper())
                add(coilBackgroundThumbnailFetcherFactory)
                add(coilRomIconFetcherFactory)
            }
            .memoryCache {
                coil.memory.MemoryCache.Builder(context)
                    .maxSizePercent(0.25)
                    .build()
            }
            .diskCache {
                coil.disk.DiskCache.Builder()
                    .directory(context.cacheDir.resolve("image_cache"))
                    .maxSizeBytes(96L * 1024 * 1024)
                    .build()
            }
            .crossfade(true)
            .build()
    }
}
