package me.magnum.melonds.ui.emulator.input

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.view.Surface
import androidx.core.content.ContextCompat
import androidx.core.content.getSystemService
import me.magnum.melonds.MelonEmulator

internal class EmulatorMotionManager(context: Context) {
    private val display = ContextCompat.getDisplayOrDefault(context)
    private val deviceSensors = context.getSystemService<SensorManager>()
    private var source: SensorManager? = null
    private var listener: SensorEventListener? = null
    private val acceleration = floatArrayOf(0f, 0f, SensorManager.GRAVITY_EARTH)
    private val rotation = FloatArray(3)

    suspend fun track(controllers: ConnectedControllerManager) {
        try {
            controllers.managedControllers.collect { devices ->
                releaseSensors()
                resetMotion()
                val controllerSensors = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    devices.firstNotNullOfOrNull { device ->
                        device.sensorManager.takeIf { it.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null }
                    }
                } else null
                if (controllerSensors == null || !register(controllerSensors, controller = true)) {
                    deviceSensors?.let { register(it, controller = false) }
                }
            }
        } finally {
            releaseSensors()
            resetMotion()
        }
    }

    private fun register(manager: SensorManager, controller: Boolean): Boolean {
        val candidate = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                if (listener !== this) return
                val target = if (event.sensor.type == Sensor.TYPE_ACCELEROMETER) acceleration else rotation
                val x = event.values[0]
                val y = event.values[1]
                val z = event.values[2]
                if (controller) {
                    target[0] = x
                    target[1] = -z
                    target[2] = y
                } else {
                    when (display.rotation) {
                        Surface.ROTATION_90 -> {
                            target[0] = -y
                            target[1] = x
                        }
                        Surface.ROTATION_180 -> {
                            target[0] = -x
                            target[1] = -y
                        }
                        Surface.ROTATION_270 -> {
                            target[0] = y
                            target[1] = -x
                        }
                        else -> {
                            target[0] = x
                            target[1] = y
                        }
                    }
                    target[2] = z
                }
                publishMotion()
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
        }
        val accelerometer = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro = manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        val hasAcceleration = accelerometer != null && manager.registerListener(candidate, accelerometer, SensorManager.SENSOR_DELAY_GAME)
        if (controller && !hasAcceleration) return false
        val hasRotation = gyro != null && manager.registerListener(candidate, gyro, SensorManager.SENSOR_DELAY_GAME)
        if (!hasAcceleration && !hasRotation) return false
        source = manager
        listener = candidate
        return true
    }

    private fun releaseSensors() {
        listener?.let { source?.unregisterListener(it) }
        listener = null
        source = null
    }

    private fun resetMotion() {
        acceleration.fill(0f)
        acceleration[2] = SensorManager.GRAVITY_EARTH
        rotation.fill(0f)
        publishMotion()
    }

    private fun publishMotion() {
        MelonEmulator.updateMotionData(acceleration[0], acceleration[1], acceleration[2], rotation[0], rotation[1], rotation[2])
    }
}
