#pragma once

#include <cstdint>
#include <string>

namespace melonDS { struct WMServicePacket;
struct WMServiceInfo; }

namespace MelonDSAndroid::Multiplayer {
int Start(int mode, const char* name, const char* host, const uint8_t mac[6], int maximum = 16, uint32_t bindAddress = 0, unsigned interfaceIndex = 0);
int Stop();
bool Pause(bool paused);
std::string Snapshot();
bool Active();
void GameStarted(void* owner, bool firmware);
void GameStopped(void* owner);
bool CopyMac(uint8_t mac[6]);
void Begin(void* owner);
void End(void* owner);
int Send(void* owner, unsigned type, const uint8_t* data, int length, uint64_t timestamp, uint16_t aid = 0);
int Receive(void* owner, bool host, uint8_t* data, uint64_t* timestamp);
uint16_t Replies(void* owner, uint8_t* data, uint64_t timestamp, uint16_t aidMask);
bool ConservativeAck(void* owner);
melonDS::WMServiceInfo WMInfo(void* owner);
void WMAbort(void* owner);
uint16_t WMSend(void* owner, uint16_t peers, const uint8_t* data, uint16_t length);
int WMReceive(void* owner, melonDS::WMServicePacket& packet);
void WMReset(void* owner);
}
