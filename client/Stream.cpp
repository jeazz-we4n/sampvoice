#include "Stream.h"

#include <audio/bass.h>
#include <util/Logger.h>

#include "SetController.h"
#include "SlideController.h"

Stream::Stream(const DWORD streamFlags, const StreamType type,
               const D3DCOLOR color, std::string name, const float distance) noexcept
    : streamFlags(streamFlags)
    , streamInfo(type, color, std::move(name), distance)
{}

const StreamInfo& Stream::GetInfo() const noexcept
{
    return this->streamInfo;
}

void Stream::Tick() noexcept
{
    for (const auto& channel : this->channels)
    {
        if (channel->HasSpeaker() && !channel->IsActive())
            channel->Reset();
    }
}

void Stream::Push(const VoicePacket& packet)
{
    const ChannelPtr* channelPtr { nullptr };

    for (const auto& channel : this->channels)
    {
        if (channel->GetSpeaker() == packet.sender)
        {
            channelPtr = &channel;
            break;
        }
    }

    if (channelPtr == nullptr)
    {
        for (const auto& channel : this->channels)
        {
            if (!channel->HasSpeaker())
            {
                Logger::LogToFile("[sv:dbg:stream:push] : channel %p was occupied by player %hu "
                    "(stream:%s)", channel.get(), packet.sender, this->streamInfo.GetName().c_str());

                channel->SetSpeaker(packet.sender);

                channelPtr = &channel;
                break;
            }
        }
    }

    if (channelPtr == nullptr)
    {
        auto& channelRef = this->channels.emplace_back(MakeChannel(this->streamFlags));
        channelRef->SetPlayCallback(std::bind(&Stream::OnChannelPlay, this, std::placeholders::_1));
        channelRef->SetStopCallback(std::bind(&Stream::OnChannelStop, this, std::placeholders::_1));
        channelRef->SetSpeaker(packet.sender);

        Logger::LogToFile("[sv:dbg:stream:push] : channel %p for player %hu created (stream:%s)",
            channelRef.get(), packet.sender, this->streamInfo.GetName().c_str());

        this->OnChannelCreate(*channelRef);

        channelPtr = &channelRef;
    }

    (*channelPtr)->Push(packet.packid, packet.data, packet.length);
}

void Stream::Reset() noexcept
{
    for (const auto& channel : this->channels)
    {
        channel->Reset();
    }
}

void Stream::SetParameter(const BYTE parameter, const float value)
{
    const auto& parameterPtr = this->parameters[parameter] =
        MakeSetController(parameter, value);

    for (const auto& channel : this->channels)
    {
        parameterPtr->Apply(*channel);
    }
}

void Stream::SlideParameter(const BYTE parameter, const float startValue,
                            const float endValue, const DWORD time)
{
    const auto& parameterPtr = this->parameters[parameter] =
        MakeSlideController(parameter, startValue, endValue, time);

    for (const auto& channel : this->channels)
    {
        parameterPtr->Apply(*channel);
    }
}

void Stream::EffectCreate(const DWORD effect, const DWORD number, const int priority,
                          const void* const paramPtr, const DWORD paramSize)
{
    const auto& effectPtr = this->effects[effect] =
        MakeEffect(number, priority, paramPtr, paramSize);

    for (const auto& channel : this->channels)
    {
        effectPtr->Apply(*channel);
    }
}

void Stream::EffectDelete(const DWORD effect)
{
    this->effects.erase(effect);
}

std::size_t Stream::AddPlayCallback(PlayCallback playCallback)
{
    for (std::size_t i { 0 }; i < this->playCallbacks.size(); ++i)
    {
        if (this->playCallbacks[i] == nullptr)
        {
            this->playCallbacks[i] = std::move(playCallback);
            return i;
        }
    }

    this->playCallbacks.emplace_back(std::move(playCallback));
    return this->playCallbacks.size() - 1;
}

std::size_t Stream::AddStopCallback(StopCallback stopCallback)
{
    for (std::size_t i { 0 }; i < this->stopCallbacks.size(); ++i)
    {
        if (this->stopCallbacks[i] == nullptr)
        {
            this->stopCallbacks[i] = std::move(stopCallback);
            return i;
        }
    }

    this->stopCallbacks.emplace_back(std::move(stopCallback));
    return this->stopCallbacks.size() - 1;
}

void Stream::RemovePlayCallback(const std::size_t callback) noexcept
{
    if (callback >= this->playCallbacks.size())
        return;

    this->playCallbacks[callback] = nullptr;
}

void Stream::RemoveStopCallback(const std::size_t callback) noexcept
{
    if (callback >= this->stopCallbacks.size())
        return;

    this->stopCallbacks[callback] = nullptr;
}

void Stream::OnChannelCreate(const Channel& channel)
{
    BASS_ChannelSetAttribute(channel.GetHandle(), BASS_ATTRIB_VOL, 4.f);

    for (const auto& parameter : this->parameters)
    {
        parameter.second->Apply(channel);
    }

    for (const auto& effect : this->effects)
    {
        effect.second->Apply(channel);
    }
}

void Stream::OnChannelPlay(const Channel& channel) noexcept
{
    if (channel.HasSpeaker())
    {
        for (const auto& playCallback : this->playCallbacks)
        {
            if (playCallback != nullptr) playCallback(*this, channel.GetSpeaker());
        }
    }
}

void Stream::OnChannelStop(const Channel& channel) noexcept
{
    if (channel.HasSpeaker())
    {
        for (const auto& stopCallback : this->stopCallbacks)
        {
            if (stopCallback != nullptr) stopCallback(*this, channel.GetSpeaker());
        }
    }
}

void Stream::SetRadioEffect(bool enable) {
    // Nếu trạng thái không đổi thì không làm gì cả
    if (this->isRadioActive == enable) return;
    this->isRadioActive = enable;

    if (enable) {
        // 1. Cắt Treble (Giảm dải âm cao)
        BASS_DX8_PARAMEQ pHigh;
        pHigh.fBandwidth = 12.0f;
        pHigh.fCenter = 4000.0f;
        pHigh.fGain = -15.0f;
        // Tham số: ID tự định nghĩa, Loại BASS Effect, Priority, Con trỏ param, Size
        this->EffectCreate(0xFF01, BASS_FX_DX8_PARAMEQ, 0, &pHigh, sizeof(pHigh));

        // 2. Cắt Bass (Giảm dải âm trầm)
        BASS_DX8_PARAMEQ pLow;
        pLow.fBandwidth = 12.0f;
        pLow.fCenter = 250.0f;
        pLow.fGain = -15.0f;
        this->EffectCreate(0xFF02, BASS_FX_DX8_PARAMEQ, 0, &pLow, sizeof(pLow));

        // 3. Distortion (Làm méo tiếng, tạo độ rè)
        BASS_DX8_DISTORTION pDist;
        pDist.fEdge = 20.0f;
        pDist.fGain = -5.0f;
        pDist.fPostEQCenterFrequency = 2400.0f;
        pDist.fPostEQBandwidth = 2400.0f;
        pDist.fPreLowpassCutoff = 8000.0f;
        this->EffectCreate(0xFF03, BASS_FX_DX8_DISTORTION, 0, &pDist, sizeof(pDist));

    } else {
        // Gỡ các effect ra bằng các ID đã định nghĩa
        this->EffectDelete(0xFF01);
        this->EffectDelete(0xFF02);
        this->EffectDelete(0xFF03);
    }
}

const std::vector<ChannelPtr>& Stream::GetChannels() const noexcept
{
    return this->channels;
}
