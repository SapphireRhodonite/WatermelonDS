package me.magnum.melonds.ui.emulator.model

import me.magnum.melonds.domain.model.ConsoleType
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareLaunchTest {
    @Test
    fun anExternalLaunchReusesOnlyTheMatchingConsole() {
        for (console in ConsoleType.entries) {
            val launch = LaunchArgs.Firmware(console)
            assertTrue(launch.resumes(console))
            assertFalse(launch.resumes(ConsoleType.entries.first { it != console }))
        }
    }

    @Test
    fun aNewRoomNeedsItsOwnFirmwareSession() {
        assertFalse(LaunchArgs.Firmware(ConsoleType.DS, startMultiplayerSession = true).resumes(ConsoleType.DS))
    }
}
