#include "kspch.h"
#include "ScriptAudio.h"
#include "Kans3D/Core/Utilities/StringUtils.h"
#include "Kans3D/Scene/Entity.h"
#include "ScriptEngine.h"

namespace Kans::ScriptAudio
{
    namespace
    {
        enum class Operation : int32_t
        {
            Play,
            Stop,
            Pause,
            UnPause,
            IsPlaying,
            Volume,
            Pitch,
            Loop,
            Clip,
            Event,
            Bus,
            Time,
            PlayOneShot,
            TriggerEvent,
            StopVoice,
            BusVolume,
            BusMute,
            UserBusVolume,
            UserBusMute,
            SaveUserSettings,
            LoadUserSettings,
            SpeakerMode,
            SelectDevice,
            EnumerateDevices,
            ReadDevice,
            LastDeviceChange,
            PushGenerator,
            GeneratorRoom,
            GeneratorUnderruns
        };
        constexpr int32_t Write = 1, EntityTarget = 2;
        using Data   = Scripting::AudioCommandData;
        using Status = AudioEventTriggerStatus;

        AssetID ReadAsset(const Data& data)
        {
            return data.Source && data.Local ?
                       AssetID(SourceAssetID::FromString(Utils::Uint64ToHex(data.Source)), data.Local) :
                       AssetID {};
        }

        bool CheckBus(Scene& scene, const AudioEngine& engine, const std::string& bus)
        {
            if (engine.GetBusVolumeDb(bus))
                return true;
            scene.WarnAudioOnce("unknown bus '" + bus + "'");
            return false;
        }

        int32_t Source(Scene& scene, AudioEngine& engine, Entity entity, Operation operation, Data& data)
        {
            auto&      source = entity.GetComponent<AudioSourceComponent>();
            const bool write  = (data.Flags & Write) != 0;
            switch (operation)
            {
                case Operation::PushGenerator:
                    return data.SampleCount <= 0 ? 0 :
                                                   static_cast<int32_t>(engine.PushGeneratorFrames(
                                                       source.GetVoiceHandle(), data.Samples, data.SampleCount));
                case Operation::GeneratorRoom:
                    return static_cast<int32_t>(engine.GetFramesAvailableToPush(source.GetVoiceHandle()));
                case Operation::GeneratorUnderruns:
                    data.Local = engine.GetGeneratorUnderrunFrames(source.GetVoiceHandle());
                    return 1;
                case Operation::Play:
                    scene.PlayAudioSource(entity);
                    return !source.GetLastError();
                case Operation::Stop:
                    scene.StopAudioSource(entity);
                    return 1;
                case Operation::Pause:
                    scene.PauseAudioSource(entity);
                    return 1;
                case Operation::UnPause:
                    return scene.UnPauseAudioSource(entity);
                case Operation::IsPlaying:
                    return scene.IsAudioSourcePlaying(entity);
                case Operation::Volume:
                case Operation::Pitch:
                case Operation::Time: {
                    if (!write)
                    {
                        data.Value = operation == Operation::Time   ? float(scene.GetAudioSourceTime(entity)) :
                                     operation == Operation::Volume ? source.Volume :
                                                                      source.Pitch;
                        return 1;
                    }
                    if (!std::isfinite(data.Value) || data.Value < 0 ||
                        (operation == Operation::Pitch && data.Value == 0))
                        return 0;
                    if (operation == Operation::Time)
                        scene.SetAudioSourceTime(entity, data.Value);
                    else
                    {
                        (operation == Operation::Volume ? source.Volume : source.Pitch) = data.Value;
                        scene.SyncAudioSource(entity);
                    }
                    return 1;
                }
                case Operation::Loop:
                    if (write)
                    {
                        source.Loop = data.Value != 0;
                        scene.SyncAudioSource(entity);
                    }
                    else
                        data.Value = source.Loop;
                    return 1;
                case Operation::Clip:
                case Operation::Event: {
                    auto& asset = operation == Operation::Clip ? source.Clip : source.Event;
                    if (!write)
                    {
                        data.Source = asset.GetSourceID().GetUUID();
                        data.Local  = asset.GetLocalID();
                        return 1;
                    }
                    const auto id   = ReadAsset(data);
                    const auto type = operation == Operation::Clip ? AssetType::AudioClip : AssetType::AudioEvent;
                    if ((data.Source || data.Local) && scene.GetAudioAssetType(id) != type)
                    {
                        scene.WarnInvalidAudioAsset(id.ToString());
                        return 0;
                    }
                    if (asset != id)
                    {
                        scene.StopAudioSource(entity);
                        asset = id;
                    }
                    return 1;
                }
                case Operation::Bus:
                    if (write)
                    {
                        const std::string bus(data.Bus ? data.Bus : "", data.BusLength);
                        if (!CheckBus(scene, engine, bus))
                            return 0;
                        source.Bus = bus;
                        scene.SyncAudioSource(entity);
                    }
                    else
                    {
                        data.Bus       = source.Bus.data();
                        data.BusLength = int32_t(source.Bus.size());
                    }
                    return 1;
                default:
                    return 0;
            }
        }

        Status Trigger(Scene& scene, AudioEngine& engine, uint64_t entityID, Operation operation, Data& data)
        {
            Entity entity;
            if (data.Flags & EntityTarget)
            {
                entity = scene.GetEntityByUUID(entityID);
                if (!entity)
                {
                    scene.WarnAudioOnce("invalid entity " + std::to_string(entityID));
                    return Status::Invalid;
                }
            }

            AudioEventParams params;
            params.Volume = data.Volume;
            params.Pitch  = data.Pitch;
            if (!std::isfinite(params.Volume) || params.Volume < 0 || !std::isfinite(params.Pitch) || params.Pitch <= 0)
                return Status::Invalid;
            if (!entity)
            {
                if (!std::isfinite(data.Position.X) || !std::isfinite(data.Position.Y) ||
                    !std::isfinite(data.Position.Z))
                    return Status::Invalid;
                params.Position = glm::vec3(data.Position.X, data.Position.Y, data.Position.Z);
            }
            if (data.Priority >= 0 && data.Priority <= UINT8_MAX)
                params.Priority = uint8_t(data.Priority);
            if (data.Bus)
            {
                params.Bus.emplace(data.Bus, data.BusLength);
                if (!CheckBus(scene, engine, *params.Bus))
                    return Status::Invalid;
            }

            const auto asset = ReadAsset(data);
            if (operation == Operation::TriggerEvent && scene.GetAudioAssetType(asset) != AssetType::AudioEvent)
            {
                scene.WarnInvalidAudioAsset(asset.ToString());
                return Status::Invalid;
            }
            const auto result = scene.PlayAudioAsset(asset, params, entity);
            if (result.Voice.IsValid())
            {
                data.Engine          = engine.GetInstanceID();
                data.VoiceIndex      = result.Voice.Index;
                data.VoiceGeneration = result.Voice.Generation;
            }
            return result.Status;
        }

        void WriteOutputFormat(const AudioOutputFormat& format, Data& data)
        {
            data.Source = format.Channels;
            data.Local  = 0;
            for (size_t i = 0; i < format.ChannelMap.size(); ++i)
                data.Local |= uint64_t(format.ChannelMap[i]) << (i * 8);
        }

        int32_t OutputSettings(AudioEngine& engine, Operation operation, Data& data)
        {
            static thread_local std::vector<AudioOutputDevice> devices;
            switch (operation)
            {
                case Operation::UserBusVolume:
                case Operation::UserBusMute: {
                    const std::string bus(data.Bus ? data.Bus : "", data.BusLength);
                    if (data.Flags & Write)
                        return operation == Operation::UserBusVolume ? engine.SetUserBusVolumeDb(bus, data.Value) :
                                                                       engine.SetUserBusMute(bus, data.Value != 0);
                    const auto settings = engine.GetUserBusSettings(bus);
                    if (!settings)
                        return 0;
                    data.Value = operation == Operation::UserBusVolume ? settings->VolumeDb : float(settings->Mute);
                    return 1;
                }
                case Operation::SaveUserSettings:
                    return !engine.SaveUserSettings();
                case Operation::LoadUserSettings:
                    return !engine.LoadUserSettings();
                case Operation::SpeakerMode:
                    if (data.Flags & Write)
                        return engine.SetSpeakerMode(static_cast<SpeakerMode>(data.Priority));
                    data.Priority = static_cast<int32_t>(engine.GetSpeakerMode());
                    return 1;
                case Operation::SelectDevice:
                    if (data.Flags & Write)
                        return engine.SetOutputDevice(std::string_view(data.Bus ? data.Bus : "", data.BusLength));
                    data.Bus       = engine.GetUserSettings().OutputDevice.data();
                    data.BusLength = static_cast<int32_t>(engine.GetUserSettings().OutputDevice.size());
                    return 1;
                case Operation::EnumerateDevices:
                    devices = engine.EnumerateOutputDevices();
                    return static_cast<int32_t>(devices.size());
                case Operation::ReadDevice: {
                    if (data.Priority < 0 || static_cast<size_t>(data.Priority) >= devices.size())
                        return 0;
                    const auto& device    = devices[data.Priority];
                    data.Bus              = device.Id.data();
                    data.BusLength        = static_cast<int32_t>(device.Id.size());
                    data.DeviceName       = device.Name.data();
                    data.DeviceNameLength = static_cast<int32_t>(device.Name.size());
                    data.Flags            = device.IsDefault;
                    WriteOutputFormat(device.NativeFormat, data);
                    return 1;
                }
                case Operation::LastDeviceChange: {
                    const auto& change   = engine.GetLastDeviceChange();
                    const auto  instance = engine.GetInstanceID();
                    if (!change.Revision || (data.Engine == instance && data.VoiceIndex == uint32_t(change.Revision) &&
                                             data.VoiceGeneration == uint32_t(change.Revision >> 32)))
                        return 0;
                    data.Engine          = instance;
                    data.VoiceIndex      = uint32_t(change.Revision);
                    data.VoiceGeneration = uint32_t(change.Revision >> 32);
                    data.Priority        = int32_t(change.Reason);
                    data.Bus             = change.DeviceId.data();
                    data.BusLength       = static_cast<int32_t>(change.DeviceId.size());
                    WriteOutputFormat(change.Format, data);
                    return 1;
                }
                default:
                    return 0;
            }
        }

        int32_t KANS_SCRIPT_CALL Command(uint64_t entityID, int32_t command, Data* data) noexcept
        {
            const auto operation = Operation(command);
            const bool trigger   = operation == Operation::PlayOneShot || operation == Operation::TriggerEvent;
            const auto failure   = trigger ? int32_t(Status::Invalid) : 0;
            try
            {
                auto* scene  = ScriptEngine::GetSceneContext();
                auto* engine = scene ? scene->GetAudioEngine() : nullptr;
                if (!data || !engine || !engine->IsInitialized() || data->BusLength < 0)
                    return failure;
                if (trigger)
                    return int32_t(Trigger(*scene, *engine, entityID, operation, *data));
                if (operation == Operation::BusVolume || operation == Operation::BusMute)
                {
                    const std::string bus(data->Bus ? data->Bus : "", data->BusLength);
                    if (!CheckBus(*scene, *engine, bus))
                        return 0;
                    if (operation == Operation::BusMute)
                        return engine->SetBusMute(bus, data->Value != 0);
                    if (data->Flags & Write)
                        return engine->SetBusVolumeDb(bus, data->Value);
                    data->Value = *engine->GetBusVolumeDb(bus);
                    return 1;
                }
                if (operation >= Operation::UserBusVolume && operation <= Operation::LastDeviceChange)
                    return OutputSettings(*engine, operation, *data);
                if (operation == Operation::StopVoice)
                {
                    const AudioVoiceHandle voice {data->VoiceIndex, data->VoiceGeneration};
                    if (data->Engine != engine->GetInstanceID() || !engine->IsVoiceValid(voice))
                    {
                        scene->WarnAudioOnce("invalid voice " + std::to_string(data->Engine) + '/' +
                                             std::to_string(voice.Index) + '/' + std::to_string(voice.Generation));
                        return 0;
                    }
                    if (!std::isfinite(data->Value) || data->Value < 0)
                        return 0;
                    if (data->Value == 0 || !engine->IsPlaying(voice))
                        engine->Stop(voice);
                    else
                    {
                        const auto fade = static_cast<uint64_t>(
                            std::clamp(double(data->Value) * engine->GetSampleRate(), 1.0, double(UINT64_MAX / 4)));
                        const auto now = engine->GetSchedulingClockFrames();
                        engine->ScheduleStop(voice, now + std::min(UINT64_MAX - now, fade), fade);
                    }
                    return 1;
                }
                auto entity = scene->GetEntityByUUID(entityID);
                if (!entity || !entity.HasComponent<AudioSourceComponent>())
                {
                    scene->WarnAudioOnce("missing audio source on entity " + std::to_string(entityID));
                    return 0;
                }
                return Source(*scene, *engine, entity, operation, *data);
            }
            catch (...)
            {
                return failure;
            }
        }
    } // namespace

    void Install(Scripting::NativeApi& api) { api.AudioCommand = Command; }
} // namespace Kans::ScriptAudio
