package me.magnum.melonds.di

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.os.Vibrator
import androidx.core.content.getSystemService
import com.google.gson.Gson
import com.squareup.picasso.Picasso
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.serialization.json.Json
import me.magnum.melonds.common.romprocessors.RomFileProcessorFactory
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.common.vibration.Api26VibratorDelegate
import me.magnum.melonds.common.vibration.OldVibratorDelegate
import me.magnum.melonds.common.vibration.TouchVibrator
import me.magnum.melonds.database.MelonDatabase
import me.magnum.melonds.database.daos.RetroAchievementsDao
import me.magnum.melonds.domain.repositories.*
import me.magnum.melonds.domain.services.ConfigurationDirectoryVerifier
import me.magnum.melonds.domain.services.DSiNandManager
import me.magnum.melonds.domain.services.EmulatorLaunchPreconditionChecker
import me.magnum.melonds.impl.AndroidDSiNandManager
import me.magnum.melonds.impl.*
import me.magnum.melonds.impl.input.ControllerConfigurationFactory
import me.magnum.melonds.impl.input.DefaultControllerConfigurationFactory
import me.magnum.melonds.impl.layout.DeviceLayoutDisplayMapper
import me.magnum.melonds.impl.layout.DefaultLayoutProvider
import me.magnum.melonds.impl.layout.UILayoutProvider
import me.magnum.melonds.impl.layout.devicemapper.AynThorLayoutDisplayMapper
import me.magnum.melonds.impl.layout.devicemapper.DefaultLayoutDisplayMapper
import me.magnum.melonds.impl.retroachievements.offline.AndroidKeystoreOfflineLedgerSigner
import me.magnum.melonds.impl.retroachievements.offline.AndroidOfflineLedgerStorage
import me.magnum.melonds.impl.retroachievements.offline.AndroidOfflinePrefetchCacheStorage
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerRepository
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerSigner
import me.magnum.melonds.impl.retroachievements.offline.OfflineLedgerStorage
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheRepository
import me.magnum.melonds.impl.retroachievements.offline.OfflinePrefetchCacheStorage
import me.magnum.melonds.impl.retroachievements.offline.RAApiSmartSyncRaClient
import me.magnum.melonds.impl.retroachievements.offline.SmartSyncEngine
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointProvider
import me.magnum.melonds.impl.romprocessors.Api24RomFileProcessorFactory
import me.magnum.melonds.ui.romdetails.RomDetailsUiMapper
import me.magnum.rcheevosapi.RAApi
import me.magnum.rcheevosapi.RAUserAuthStore
import me.magnum.rcheevosapi.RAUserProfileStore
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object MelonModule {

    @Provides
    fun provideControllerConfigurationFactory(): ControllerConfigurationFactory {
        return DefaultControllerConfigurationFactory()
    }

    @Provides
    @Singleton
    fun provideSettingsRepository(@ApplicationContext context: Context, sharedPreferences: SharedPreferences, controllerConfigurationFactory: ControllerConfigurationFactory, json: Json, uriHandler: UriHandler, settingsBackupManager: SettingsBackupManager, retroArchShaderLibraryManager: RetroArchShaderLibraryManager): SettingsRepository {
        return SharedPreferencesSettingsRepository(context, sharedPreferences, controllerConfigurationFactory, json, uriHandler, CoroutineScope(Dispatchers.IO), settingsBackupManager, retroArchShaderLibraryManager)
    }

    @Provides
    @Singleton
    fun provideSettingsBackupManager(@ApplicationContext context: Context, sharedPreferences: SharedPreferences, database: MelonDatabase): SettingsBackupManager {
        return SettingsBackupManager(context, sharedPreferences, database)
    }

    @Provides
    @Singleton
    fun provideRomsRepository(@ApplicationContext context: Context, gson: Gson, settingsRepository: SettingsRepository, romFileProcessorFactory: RomFileProcessorFactory, uriHandler: UriHandler, settingsBackupManager: SettingsBackupManager): RomsRepository {
        return FileSystemRomsRepository(context, gson, settingsRepository, romFileProcessorFactory, uriHandler, settingsBackupManager)
    }

    @Provides
    @Singleton
    fun provideCheatsRepository(database: MelonDatabase, settingsBackupManager: SettingsBackupManager): CheatsRepository {
        return RoomCheatsRepository(database, settingsBackupManager)
    }

    @Provides
    @Singleton
    fun provideNdsRomCache(@ApplicationContext context: Context, settingsRepository: SettingsRepository): NdsRomCache {
        return NdsRomCache(context, settingsRepository)
    }

    @Provides
    @Singleton
    fun provideLayoutsRepository(@ApplicationContext context: Context, gson: Gson, settingsBackupManager: SettingsBackupManager): LayoutsRepository {
        return InternalLayoutsRepository(context, gson, settingsBackupManager)
    }

    @Provides
    @Singleton
    fun provideBackgroundsRepository(@ApplicationContext context: Context, gson: Gson, settingsBackupManager: SettingsBackupManager): BackgroundRepository {
        return InternalBackgroundsRepository(context, gson, settingsBackupManager)
    }

    @Provides
    @Singleton
    fun provideSaveStatesRepository(settingsRepository: SettingsRepository, saveStateScreenshotProvider: SaveStateScreenshotProvider, uriHandler: UriHandler, @ApplicationContext context: Context): SaveStatesRepository {
        return FileSystemSaveStatesRepository(settingsRepository, saveStateScreenshotProvider, uriHandler, context)
    }

    @Provides
    @Singleton
    fun provideDSiWareMetadataRepository(): DSiWareMetadataRepository {
        return NusDSiWareMetadataRepository()
    }

    @Provides
    @Singleton
    fun provideRetroAchievementsRepository(
        raApi: RAApi,
        retroAchievementsDao: RetroAchievementsDao,
        raUserAuthStore: RAUserAuthStore,
        raUserProfileStore: RAUserProfileStore,
        sharedPreferences: SharedPreferences,
        endpointProvider: RetroAchievementsEndpointProvider,
        @ApplicationContext context: Context,
    ): RetroAchievementsRepository {
        return AndroidRetroAchievementsRepository(
            raApi,
            retroAchievementsDao,
            raUserAuthStore,
            raUserProfileStore,
            sharedPreferences,
            context,
            endpointProvider,
        )
    }

    @Provides
    @Singleton
    fun provideOfflineLedgerSigner(): OfflineLedgerSigner {
        return AndroidKeystoreOfflineLedgerSigner()
    }

    @Provides
    @Singleton
    fun provideOfflineLedgerStorage(storage: AndroidOfflineLedgerStorage): OfflineLedgerStorage {
        return storage
    }

    @Provides
    @Singleton
    fun provideOfflinePrefetchCacheStorage(storage: AndroidOfflinePrefetchCacheStorage): OfflinePrefetchCacheStorage {
        return storage
    }

    @Provides
    @Singleton
    fun provideOfflinePrefetchCacheRepository(storage: OfflinePrefetchCacheStorage): OfflinePrefetchCacheRepository {
        return OfflinePrefetchCacheRepository(storage)
    }

    @Provides
    @Singleton
    fun provideOfflineLedgerRepository(
        storage: OfflineLedgerStorage,
        signer: OfflineLedgerSigner,
        endpointProvider: RetroAchievementsEndpointProvider,
    ): OfflineLedgerRepository {
        return OfflineLedgerRepository(
            storage = storage,
            signer = signer,
            writesEnabled = { endpointProvider.currentSnapshot().builtInLedgerEnabled },
        )
    }

    @Provides
    @Singleton
    fun provideSmartSyncEngine(
        raApi: RAApi,
        ledgerRepository: OfflineLedgerRepository,
        prefetchCacheRepository: OfflinePrefetchCacheRepository,
        endpointProvider: RetroAchievementsEndpointProvider,
    ): SmartSyncEngine {
        return SmartSyncEngine(
            raClient = RAApiSmartSyncRaClient(raApi),
            ledgerRepository = ledgerRepository,
            prefetchCacheRepository = prefetchCacheRepository,
            syncEnabled = { endpointProvider.currentSnapshot().builtInSyncEnabled },
        )
    }

    @Provides
    @Singleton
    fun provideConfigurationDirectoryVerifier(@ApplicationContext context: Context, settingsRepository: SettingsRepository): ConfigurationDirectoryVerifier {
        return FileSystemConfigurationDirectoryVerifier(context, settingsRepository)
    }

    @Provides
    @Singleton
    fun provideFileRomProcessorFactory(@ApplicationContext context: Context, uriHandler: UriHandler, ndsRomCache: NdsRomCache): RomFileProcessorFactory {
        return Api24RomFileProcessorFactory(context, uriHandler, ndsRomCache)
    }

    @Provides
    @Singleton
    fun provideRomIconProvider(@ApplicationContext context: Context, romFileProcessorFactory: RomFileProcessorFactory): RomIconProvider {
        return RomIconProvider(context, romFileProcessorFactory)
    }

    @Provides
    @Singleton
    fun provideSaveStateScreenshotProvider(@ApplicationContext context: Context, picasso: Picasso): SaveStateScreenshotProvider {
        return SaveStateScreenshotProvider(context, picasso)
    }

    @Provides
    @Singleton
    fun provideBackgroundThumbnailProvider(@ApplicationContext context: Context): BackgroundThumbnailProvider {
        return BackgroundThumbnailProvider(context)
    }

    @Provides
    @Singleton
    fun provideTouchVibrator(@ApplicationContext context: Context, settingsRepository: SettingsRepository): TouchVibrator {
        val vibrator = context.getSystemService<Vibrator>()!!
        val delegate = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Api26VibratorDelegate(vibrator)
        } else {
            OldVibratorDelegate(vibrator)
        }
        return TouchVibrator(delegate, settingsRepository)
    }

    @Provides
    @Singleton
    fun provideScreenUnitsConverter(@ApplicationContext context: Context): ScreenUnitsConverter {
        return ScreenUnitsConverter(context)
    }

    @Provides
    @Singleton
    fun provideDeviceLayoutDisplayMapper(@ApplicationContext context: Context): DeviceLayoutDisplayMapper {
        return if (Build.MANUFACTURER == "AYN" && Build.MODEL == "AYN Thor") {
            AynThorLayoutDisplayMapper(context)
        } else {
            DefaultLayoutDisplayMapper(context)
        }
    }

    @Provides
    @Singleton
    fun provideDefaultLayoutBuilder(@ApplicationContext context: Context, screenUnitsConverter: ScreenUnitsConverter): DefaultLayoutProvider {
        return DefaultLayoutProvider(context, screenUnitsConverter)
    }

    @Provides
    fun provideUILayoutProvider(defaultLayoutProvider: DefaultLayoutProvider): UILayoutProvider {
        return UILayoutProvider(defaultLayoutProvider)
    }

    @Provides
    @Singleton
    fun provideDSiNandManager(
        @ApplicationContext context: Context,
        dSiWareMetadataRepository: DSiWareMetadataRepository,
    ): DSiNandManager {
        return AndroidDSiNandManager(context, dSiWareMetadataRepository)
    }

    @Provides
    @Singleton
    fun provideRomDetailsUiMapper(@ApplicationContext context: Context, layoutsRepository: LayoutsRepository): RomDetailsUiMapper {
        return RomDetailsUiMapper(context, layoutsRepository)
    }

    @Provides
    fun provideRomLaunchPreconditionChecker(
        @ApplicationContext context: Context,
        configurationDirectoryVerifier: ConfigurationDirectoryVerifier,
        romFileProcessorFactory: RomFileProcessorFactory,
        dsiNandManager: DSiNandManager,
        settingsRepository: SettingsRepository,
    ): EmulatorLaunchPreconditionChecker {
        return EmulatorLaunchPreconditionChecker(
            context = context,
            configurationDirectoryVerifier = configurationDirectoryVerifier,
            romFileProcessorFactory = romFileProcessorFactory,
            dsiNandManager = dsiNandManager,
            settingsRepository = settingsRepository,
        )
    }
}
