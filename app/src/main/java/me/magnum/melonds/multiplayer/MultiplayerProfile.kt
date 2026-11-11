package me.magnum.melonds.multiplayer

import android.content.Context
import androidx.preference.PreferenceManager
import org.json.JSONObject

data class MultiplayerProfile(
    val nickname: String,
    val message: String,
    val colour: Int,
    val symbol: String,
    val birthdayDay: Int,
    val birthdayMonth: Int,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("name", nickname)
        .put("message", message)
        .put("colour", colour)
        .put("symbol", symbol)

    companion object {
        const val NICKNAME_LENGTH = 10
        const val MESSAGE_LENGTH = 26
        val SYMBOLS = listOf("", "star", "heart", "note", "bolt", "moon", "crown", "smile", "flower", "leaf", "diamond", "pad")

        fun load(context: Context): MultiplayerProfile {
            val preferences = PreferenceManager.getDefaultSharedPreferences(context.applicationContext)
            val birthday = preferences.getString("firmware_settings_birthday", "01/01").orEmpty().split("/")
            return MultiplayerProfile(
                nickname = preferences.getString("firmware_settings_nickname", "Player").orEmpty().take(NICKNAME_LENGTH),
                message = preferences.getString("firmware_settings_message", "Hello!").orEmpty().take(MESSAGE_LENGTH),
                colour = preferences.getInt("firmware_settings_colour", 0).coerceIn(0, 15),
                symbol = context.applicationContext.getSharedPreferences("watermelon_multiplayer", Context.MODE_PRIVATE)
                    .getString("symbol", "star").takeIf { it in SYMBOLS }.orEmpty(),
                birthdayDay = birthday.getOrNull(0)?.toIntOrNull()?.coerceIn(1, 31) ?: 1,
                birthdayMonth = birthday.getOrNull(1)?.toIntOrNull()?.coerceIn(1, 12) ?: 1,
            )
        }

        fun save(context: Context, profile: MultiplayerProfile): Boolean {
            val nickname = profile.nickname.trim().take(NICKNAME_LENGTH)
            if (nickname.isEmpty()) return false
            val saved = PreferenceManager.getDefaultSharedPreferences(context.applicationContext).edit()
                .putString("firmware_settings_nickname", nickname)
                .putString("firmware_settings_message", profile.message.take(MESSAGE_LENGTH))
                .putInt("firmware_settings_colour", profile.colour.coerceIn(0, 15))
                .putString("firmware_settings_birthday", "%02d/%02d".format(profile.birthdayDay, profile.birthdayMonth))
                .commit()
            return saved && context.applicationContext.getSharedPreferences("watermelon_multiplayer", Context.MODE_PRIVATE)
                .edit().putString("symbol", profile.symbol.takeIf { it in SYMBOLS }.orEmpty()).commit()
        }

        fun fromJson(value: JSONObject): MultiplayerProfile = MultiplayerProfile(
            nickname = value.optString("name").take(NICKNAME_LENGTH),
            message = value.optString("message").take(MESSAGE_LENGTH),
            colour = value.optInt("colour").coerceIn(0, 15),
            symbol = value.optString("symbol").takeIf { it in SYMBOLS }.orEmpty(),
            birthdayDay = 1,
            birthdayMonth = 1,
        )
    }
}
