import com.android.build.gradle.internal.cxx.configure.gradleLocalProperties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.hilt.android)
    alias(libs.plugins.kotlin.parcelize)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
}

data class LibrashaderAbiTarget(
    val abi: String,
    val rustTarget: String,
    val ndkLibTriple: String,
    val clangPrefix: String,
)

android {
    signingConfigs {
        create("release") {
            val props = gradleLocalProperties(rootDir, providers)
            (props["MELONDS_KEYSTORE"] as String?)?.let { storeFile = file(it) }
            storePassword = props["MELONDS_KEYSTORE_PASSWORD"] as String? ?: ""
            keyAlias = props["MELONDS_KEY_ALIAS"] as String? ?: ""
            keyPassword = props["MELONDS_KEY_PASSWORD"] as String? ?: ""
        }
    }

    namespace = "me.magnum.melonds"
    compileSdk = AppConfig.compileSdkVersion
    ndkVersion = AppConfig.ndkVersion
    defaultConfig {
        applicationId = "me.magnum.melondualds"
        minSdk = AppConfig.minSdkVersion
        targetSdk = AppConfig.targetSdkVersion
        versionCode = AppConfig.versionCode
        versionName = AppConfig.versionName
        manifestPlaceholders["appName"] = "@string/app_name"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a", "x86_64"))
        }
        externalNativeBuild {
            cmake {
                cppFlags("-std=c++17 -Wno-write-strings")
            }
        }
        vectorDrawables.useSupportLibrary = true
    }
    buildFeatures {
        viewBinding = true
        compose = true
        resValues = true
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("release")
            externalNativeBuild {
                cmake {
                    arguments("-DMELONDS_ANDROID_DEBUG_BUILD=0")
                }
            }
        }
        getByName("debug") {
            applicationIdSuffix = ".dev"
            externalNativeBuild {
                cmake {
                    arguments("-DMELONDS_ANDROID_DEBUG_BUILD=1")
                }
            }
        }
    }

    flavorDimensions.add("version")
    flavorDimensions.add("build")
    productFlavors {
        create("playStore") {
            dimension = "version"
            resValue("bool", "adrenotools_enabled", "false")
            externalNativeBuild {
                cmake {
                    arguments("-DMELONDS_ENABLE_ADRENOTOOLS=0")
                }
            }
        }
        create("gitHub") {
            dimension = "version"
            isDefault = true
            resValue("bool", "adrenotools_enabled", "true")
            externalNativeBuild {
                cmake {
                    arguments("-DMELONDS_ENABLE_ADRENOTOOLS=1")
                }
            }
        }

        create("prod") {
            dimension = "build"
            isDefault = true
        }
        create("nightly") {
            dimension = "build"
            applicationIdSuffix = ".nightly"
        }
    }
    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }
    sourceSets {
        // Adds exported schema location as test app assets.
        getByName("androidTest").assets.directories += "$projectDir/schemas"
        getByName("main").jniLibs.srcDir("$buildDir/generated/librashader/jniLibs")
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
}

androidComponents {
    onVariants(selector().withName("gitHubProdDebug")) { variant ->
        // Keep prod-debug distinct from other debug/release variants.
        variant.manifestPlaceholders.put("appName", "debug melonDualDS")
        variant.sources.res?.addStaticSourceDirectory("src/nightly/res")
    }
}

val vulkanShaderSources = listOf(
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_InterpSpansShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_BinCombinedShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_CalculateWorkOffsetsShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_SortWorkShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterBaseShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterCompatShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_DepthBlendShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_FinalPassShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_CaptureLineExportShader.comp"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.vert"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShader.frag"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsNoColorShader.frag"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsClearShader.frag"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFinalShader.vert"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeShader.frag"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeFogShader.frag"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFogShader.frag"),
    rootProject.file("app/src/main/cpp/renderer/VulkanCompositorShader.comp"),
    rootProject.file("app/src/main/cpp/renderer/VulkanCompositorCompatibilityShader.comp"),
    rootProject.file("app/src/main/cpp/renderer/VulkanAccumulate3dShader.comp"),
    rootProject.file("app/src/main/cpp/renderer/VulkanAccumulate3dCompatibilityShader.comp"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenter.vert"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenter.frag"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenterCompatibility.frag"),
)

val vulkanShaderHeaders = listOf(
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_InterpSpansShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_BinCombinedShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_CalculateWorkOffsetsShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_SortWorkShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterBaseShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_TriRasterCompatShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_DepthBlendShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_FinalPassShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_CaptureLineExportShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShaderVertexData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateToonShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulatePlainShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaToonShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainShaderFragmentData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsNoColorShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsClearShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFinalShaderVertexData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsEdgeFogShaderData.h"),
    rootProject.file("melonDS-android-lib/src/GPU3D_Vulkan_GraphicsFogShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanCompositorShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanCompositorCompatibilityShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanAccumulate3dShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanAccumulate3dCompatibilityShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenterVertexShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenterFragmentShaderData.h"),
    rootProject.file("app/src/main/cpp/renderer/VulkanSurfacePresenterCompatibilityFragmentShaderData.h"),
)

val regenerateVulkanSpirv by tasks.registering(Exec::class) {
    group = "build"
    description = "Regenerates embedded Vulkan SPIR-V headers from Vulkan GLSL sources."
    executable = "bash"
    args(rootProject.file("scripts/regenerate_vulkan_spirv.sh").absolutePath)
    workingDir = rootProject.projectDir

    inputs.file(rootProject.file("scripts/regenerate_vulkan_spirv.sh"))
    inputs.files(vulkanShaderSources)
    outputs.files(vulkanShaderHeaders)
}

val checkVulkanSpirv by tasks.registering(Exec::class) {
    group = "verification"
    description = "Checks whether embedded Vulkan SPIR-V headers are synchronized with GLSL sources."
    executable = "bash"
    args(rootProject.file("scripts/regenerate_vulkan_spirv.sh").absolutePath, "--check")
    workingDir = rootProject.projectDir

    inputs.file(rootProject.file("scripts/regenerate_vulkan_spirv.sh"))
    inputs.files(vulkanShaderSources)
    outputs.upToDateWhen { false }
}

val librashaderRepoUrl = "https://github.com/SnowflakePowered/librashader.git"
val librashaderPinnedRevision = "76462c030b75c4f2d56e5386c3d4d7d1128318b8"
val librashaderSourceDir = layout.buildDirectory.dir("librashader/src")
val librashaderOutputDir = layout.buildDirectory.dir("generated/librashader")
val librashaderSourceRevisionFile = librashaderSourceDir.map { it.file(".melonds-librashader-revision") }
val librashaderRevisionFile = librashaderOutputDir.map { it.file("REVISION") }
val librashaderAbiTargets = listOf(
    LibrashaderAbiTarget(
        abi = "arm64-v8a",
        rustTarget = "aarch64-linux-android",
        ndkLibTriple = "aarch64-linux-android",
        clangPrefix = "aarch64-linux-android",
    ),
    LibrashaderAbiTarget(
        abi = "armeabi-v7a",
        rustTarget = "armv7-linux-androideabi",
        ndkLibTriple = "arm-linux-androideabi",
        clangPrefix = "armv7a-linux-androideabi",
    ),
    LibrashaderAbiTarget(
        abi = "x86_64",
        rustTarget = "x86_64-linux-android",
        ndkLibTriple = "x86_64-linux-android",
        clangPrefix = "x86_64-linux-android",
    ),
)

fun librashaderToolSearchDirs(): List<File> {
    val pathDirs = System.getenv("PATH")
        ?.split(File.pathSeparator)
        ?.filter(String::isNotBlank)
        ?.map(::File)
        ?: emptyList()
    val homeDir = System.getProperty("user.home")?.takeIf(String::isNotBlank)?.let(::File)
    val fallbackDirs = listOfNotNull(
        homeDir?.resolve(".cargo/bin"),
        file("/opt/homebrew/bin"),
        file("/usr/local/bin"),
        file("/usr/bin"),
        file("/bin"),
    )
    return (pathDirs + fallbackDirs).distinctBy { it.absolutePath }
}

fun augmentedLibrashaderPath(): String {
    return librashaderToolSearchDirs().joinToString(File.pathSeparator) { it.absolutePath }
}

fun resolveBuildTool(tool: String): String {
    val envOverride = System.getenv(tool.uppercase())
        ?.takeIf(String::isNotBlank)
        ?.let(::File)
    if (envOverride != null) {
        check(envOverride.isFile && envOverride.canExecute()) {
            "${tool.uppercase()} points to a non-executable file: ${envOverride.absolutePath}"
        }
        return envOverride.absolutePath
    }

    val executable = librashaderToolSearchDirs()
        .map { it.resolve(tool) }
        .firstOrNull { it.isFile && it.canExecute() }

    check(executable != null) {
        "${tool} is required to build librashader for Android. " +
            "Android Studio may not inherit your shell PATH; install Rust with rustup or set ${tool.uppercase()} to the executable path."
    }
    return executable.absolutePath
}

fun runBuildCommand(command: List<String>, workingDir: File? = null) {
    val processBuilder = ProcessBuilder(command)
        .redirectErrorStream(true)
        .inheritIO()
    processBuilder.environment()["PATH"] = augmentedLibrashaderPath()
    if (workingDir != null) {
        processBuilder.directory(workingDir)
    }

    val exitCode = processBuilder.start().waitFor()
    check(exitCode == 0) { "Command failed with exit code ${exitCode}: ${command.joinToString(" ")}" }
}

fun resolveAndroidNdkHome(): File {
    val explicitNdkHome = listOf("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT")
        .mapNotNull { System.getenv(it)?.takeIf(String::isNotBlank) }
        .firstOrNull()
    if (explicitNdkHome != null) {
        val ndkHome = file(explicitNdkHome)
        check(ndkHome.isDirectory) { "Android NDK not found at ${ndkHome.absolutePath}" }
        return ndkHome
    }

    val localProperties = gradleLocalProperties(rootDir, providers)
    (localProperties["ndk.dir"] as? String)?.takeIf(String::isNotBlank)?.let {
        val ndkHome = file(it)
        check(ndkHome.isDirectory) { "Android NDK not found at ${ndkHome.absolutePath}" }
        return ndkHome
    }

    val androidHome = System.getenv("ANDROID_HOME")
        ?: System.getenv("ANDROID_SDK_ROOT")
        ?: localProperties["sdk.dir"] as? String
    if (!androidHome.isNullOrBlank()) {
        val ndkHome = file(androidHome).resolve("ndk")
            .listFiles()
            ?.filter(File::isDirectory)
            ?.sortedBy(File::getName)
            ?.lastOrNull()
        if (ndkHome != null) {
            return ndkHome
        }
    }

    error("Android NDK not found. Set ANDROID_NDK_HOME or ANDROID_NDK_ROOT.")
}

fun androidNdkHostTag(): String {
    val osName = System.getProperty("os.name").lowercase()
    return when {
        osName.contains("linux") -> "linux-x86_64"
        osName.contains("mac") -> "darwin-x86_64"
        else -> error("Unsupported Android NDK host OS: ${System.getProperty("os.name")}")
    }
}

fun rustTargetEnvKey(rustTarget: String): String {
    return rustTarget.uppercase().replace("-", "_")
}

val prepareLibrashaderSource by tasks.registering {
    group = "build"
    description = "Checks out the pinned librashader source revision."

    inputs.property("librashaderRepoUrl", librashaderRepoUrl)
    inputs.property("librashaderPinnedRevision", librashaderPinnedRevision)
    outputs.file(librashaderSourceRevisionFile)

    doLast {
        val git = resolveBuildTool("git")
        resolveBuildTool("cargo")
        resolveBuildTool("rustup")

        val sourceDir = librashaderSourceDir.get().asFile
        if (!sourceDir.resolve(".git").isDirectory) {
            delete(sourceDir)
            runBuildCommand(listOf(git, "clone", "--filter=blob:none", librashaderRepoUrl, sourceDir.absolutePath))
        }

        runBuildCommand(listOf(git, "-C", sourceDir.absolutePath, "fetch", "--depth=1", "origin", librashaderPinnedRevision))
        runBuildCommand(listOf(git, "-C", sourceDir.absolutePath, "checkout", "--detach", librashaderPinnedRevision))

        librashaderSourceRevisionFile.get().asFile.writeText("${librashaderPinnedRevision}\n")
    }
}

val copyLibrashaderHeaders by tasks.registering(Copy::class) {
    group = "build"
    description = "Copies generated librashader C API headers."
    dependsOn(prepareLibrashaderSource)

    from(librashaderSourceDir.map { it.file("include/librashader.h") })
    from(librashaderSourceDir.map { it.file("include/librashader_ld.h") })
    into(librashaderOutputDir.map { it.dir("include") })
}

val writeLibrashaderRevision by tasks.registering {
    group = "build"
    description = "Writes the pinned librashader revision used by the Android build."

    inputs.property("librashaderPinnedRevision", librashaderPinnedRevision)
    outputs.file(librashaderRevisionFile)

    doLast {
        librashaderRevisionFile.get().asFile.writeText("${librashaderPinnedRevision}\n")
    }
}

val copyLibrashaderAbiArtifacts = librashaderAbiTargets.map { abiTarget ->
    val capitalizedAbi = abiTarget.abi
        .split('-', '_')
        .joinToString("") { it.replaceFirstChar(Char::uppercaseChar) }
    val installRustTarget = tasks.register<Exec>("installLibrashader${capitalizedAbi}RustTarget") {
        group = "build"
        description = "Installs Rust target ${abiTarget.rustTarget} for librashader."
        dependsOn(prepareLibrashaderSource)

        commandLine(resolveBuildTool("rustup"), "target", "add", abiTarget.rustTarget)
        environment("PATH", augmentedLibrashaderPath())
    }
    val compileLibrashader = tasks.register<Exec>("compileLibrashader${capitalizedAbi}") {
        group = "build"
        description = "Compiles librashader for ${abiTarget.abi}."
        dependsOn(prepareLibrashaderSource, installRustTarget)

        val hostTag = androidNdkHostTag()
        val targetEnvKey = rustTargetEnvKey(abiTarget.rustTarget)
        val ndkHome = resolveAndroidNdkHome()
        val toolchain = ndkHome.resolve("toolchains/llvm/prebuilt/${hostTag}/bin")
        val clang = toolchain.resolve("${abiTarget.clangPrefix}${AppConfig.minSdkVersion}-clang")
        val clangCpp = toolchain.resolve("${abiTarget.clangPrefix}${AppConfig.minSdkVersion}-clang++")
        val llvmAr = toolchain.resolve("llvm-ar")

        inputs.property("librashaderPinnedRevision", librashaderPinnedRevision)
        outputs.file(librashaderSourceDir.map {
            it.file("target/${abiTarget.rustTarget}/optimized/liblibrashader_capi.so")
        })

        workingDir = librashaderSourceDir.get().asFile
        commandLine(
            resolveBuildTool("cargo"),
            "+stable",
            "build",
            "--package",
            "librashader-capi",
            "--profile",
            "optimized",
            "--target",
            abiTarget.rustTarget,
            "--no-default-features",
            "--features",
            "runtime-vulkan,stable",
        )
        environment("CC_${abiTarget.rustTarget.replace("-", "_")}", clang.absolutePath)
        environment("CXX_${abiTarget.rustTarget.replace("-", "_")}", clangCpp.absolutePath)
        environment("AR_${abiTarget.rustTarget.replace("-", "_")}", llvmAr.absolutePath)
        environment("CARGO_TARGET_${targetEnvKey}_LINKER", clang.absolutePath)
        environment("CARGO_TARGET_${targetEnvKey}_RUSTFLAGS", "-C link-arg=-Wl,-soname,liblibrashader.so")
        environment("PATH", augmentedLibrashaderPath())

        doFirst {
            check(toolchain.isDirectory) { "Android NDK LLVM toolchain not found at ${toolchain.absolutePath}" }
            check(clang.isFile) { "Android NDK clang not found at ${clang.absolutePath}" }
            check(clangCpp.isFile) { "Android NDK clang++ not found at ${clangCpp.absolutePath}" }
            check(llvmAr.isFile) { "Android NDK llvm-ar not found at ${llvmAr.absolutePath}" }
        }
    }

    tasks.register<Copy>("copyLibrashader${capitalizedAbi}Artifacts") {
        group = "build"
        description = "Copies librashader artifacts for ${abiTarget.abi}."
        dependsOn(compileLibrashader)

        val hostTag = androidNdkHostTag()
        val ndkHome = resolveAndroidNdkHome()

        from(librashaderSourceDir.map {
            it.file("target/${abiTarget.rustTarget}/optimized/liblibrashader_capi.so")
        }) {
            rename { "liblibrashader.so" }
        }
        from(
            ndkHome.resolve(
                "toolchains/llvm/prebuilt/${hostTag}/sysroot/usr/lib/${abiTarget.ndkLibTriple}/libc++_shared.so",
            ),
        )
        into(librashaderOutputDir.map { it.dir("jniLibs/${abiTarget.abi}") })
    }
}

val buildLibrashaderAndroid by tasks.registering {
    group = "build"
    description = "Builds the pinned librashader Vulkan C API for Android ABIs."
    dependsOn(copyLibrashaderHeaders)
    dependsOn(writeLibrashaderRevision)
    dependsOn(copyLibrashaderAbiArtifacts)

    inputs.property("librashaderPinnedRevision", librashaderPinnedRevision)
    outputs.dir(layout.buildDirectory.dir("generated/librashader"))
}

tasks.named("preBuild").configure {
    dependsOn(regenerateVulkanSpirv)
    dependsOn(buildLibrashaderAndroid)
}

tasks.matching {
    it.name.startsWith("configureCMake") || it.name.startsWith("externalNativeBuild")
}.configureEach {
    dependsOn(buildLibrashaderAndroid)
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_21
        freeCompilerArgs.add("-opt-in=kotlin.ExperimentalUnsignedTypes")
    }

    ksp {
        arg("room.schemaLocation", "$projectDir/schemas")
    }
}

dependencies {
    val gitHubImplementation by configurations

    implementation(projects.masterswitch)
    implementation(projects.rcheevosApi)
    implementation(projects.common)

    implementation(libs.androidx.activity)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.cardview)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.fragment)
    implementation(libs.androidx.hilt.work)
    implementation(libs.androidx.lifecycle.viewmodel)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.preference)
    implementation(libs.androidx.recyclerview)
    implementation(libs.androidx.room)
    implementation(libs.androidx.room.ktx)
    implementation(libs.androidx.room.rxjava)
    implementation(libs.androidx.splashscreen)
    implementation(libs.androidx.startup)
    implementation(libs.androidx.swiperefreshlayout)
    implementation(libs.androidx.window)
    implementation(libs.androidx.work)
    implementation(libs.android.material)

    implementation(platform(libs.compose.bom))
    implementation(libs.compose.foundation)
    implementation(libs.compose.material)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons)
    implementation(libs.compose.navigation)
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.tooling.preview)

    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.coil)
    implementation(libs.gson)
    implementation(libs.hilt)
    implementation(libs.kotlin.serialization)
    implementation(libs.kotlin.serialization.protobuf)
    implementation(libs.androidx.security.crypto)
    implementation(libs.kotlinx.coroutines.rx)
    implementation(libs.picasso)
    implementation(libs.markwon)
    implementation(libs.markwon.imagepicasso)
    implementation(libs.markwon.linkify)
    implementation(libs.commons.compress)
    implementation(libs.xz)

    gitHubImplementation(libs.retrofit)
    gitHubImplementation(libs.retrofit.converter.kotlinx)

    ksp(libs.hilt.compiler)
    ksp(libs.hilt.compiler.android)
    ksp(libs.room.compiler)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)

    androidTestImplementation(libs.androidx.room.testing)
    androidTestImplementation(libs.androidx.test.core)
    androidTestImplementation(libs.androidx.test.junit)
    androidTestImplementation(libs.androidx.test.runner)
}
