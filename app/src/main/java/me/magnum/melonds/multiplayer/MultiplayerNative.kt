package me.magnum.melonds.multiplayer

import me.magnum.melonds.NativeCoreLoader

object MultiplayerNative {
    init { NativeCoreLoader.load() }
    external fun start(mode: Int, name: ByteArray, address: ByteArray, mac: ByteArray, maximum: Int, localAddress: ByteArray, interfaceIndex: Int): Int
    external fun bindInterface(fd: Int, index: Int): Boolean
    external fun stop(): Int
    external fun active(): Boolean
    external fun status(): ByteArray
}
