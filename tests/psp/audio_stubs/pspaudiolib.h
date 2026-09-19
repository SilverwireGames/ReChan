#pragma once
constexpr int PSP_VOLUME_MAX = 0x8000;
inline int pspAudioInit() { return 0; }
inline void pspAudioEnd() {}
template<class... T> inline void pspAudioSetChannelCallback(T...) {}
template<class... T> inline void pspAudioSetVolume(T...) {}
