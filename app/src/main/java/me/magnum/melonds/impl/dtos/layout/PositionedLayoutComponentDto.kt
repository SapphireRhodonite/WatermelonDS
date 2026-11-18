package me.magnum.melonds.impl.dtos.layout

import com.google.gson.annotations.SerializedName
import me.magnum.melonds.domain.model.layout.PositionedLayoutComponent
import me.magnum.melonds.domain.model.layout.ScreenAspectRatio
import me.magnum.melonds.domain.model.layout.VirtualButtonMode
import me.magnum.melonds.utils.enumValueOfIgnoreCase

data class PositionedLayoutComponentDto(
    @SerializedName("rect")
    val rect: RectDto,
    @SerializedName("component")
    val component: String,
    @SerializedName("alpha")
    val alpha: Float? = null,
    @SerializedName("onTop")
    val onTop: Boolean? = null,
    @SerializedName("buttonMode")
    val buttonMode: String? = null,
    @SerializedName("screenAspectRatio")
    val screenAspectRatio: String? = null,
) {

    companion object {
        fun fromModel(positionedLayoutComponent: PositionedLayoutComponent): PositionedLayoutComponentDto {
            return PositionedLayoutComponentDto(
                RectDto.fromModel(positionedLayoutComponent.rect),
                positionedLayoutComponent.component.name,
                positionedLayoutComponent.alpha,
                positionedLayoutComponent.onTop,
                positionedLayoutComponent.buttonMode.takeUnless { it == VirtualButtonMode.NORMAL }?.name,
                positionedLayoutComponent.screenAspectRatio?.name,
            )
        }
    }

    fun toModel(): PositionedLayoutComponent {
        return PositionedLayoutComponent(
            rect.toModel(),
            enumValueOfIgnoreCase(component),
            alpha ?: 1f,
            onTop ?: false,
            VirtualButtonMode.entries.firstOrNull { it.name.equals(buttonMode, ignoreCase = true) } ?: VirtualButtonMode.NORMAL,
            ScreenAspectRatio.entries.firstOrNull { it.name == screenAspectRatio },
        )
    }
}
