using System;
using System.Globalization;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Kans.Interop;

namespace Kans
{
    /// <summary>The source and local parts of a Kans asset ID.</summary>
    public readonly record struct AssetHandle(ulong Source, ulong Local)
    {
        public bool IsValid => Source != 0 && Local != 0;

        public static bool TryParse(string? text, out AssetHandle handle)
        {
            handle = default;
            if (text == null || text.Length != 33 || text[16] != ':' ||
                !ulong.TryParse(text.AsSpan(0, 16), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong source) ||
                !ulong.TryParse(text.AsSpan(17), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong local) ||
                source == 0 || local == 0)
                return false;
            handle = new AssetHandle(source, local);
            return true;
        }

        /// <summary>Returns an empty handle for malformed IDs.</summary>
        public static AssetHandle Parse(string? text) => TryParse(text, out var handle) ? handle : default;
        public override string ToString() => $"{Source:x16}:{Local:x16}";
    }

    public enum AudioEventTriggerStatus
    {
        Started, Cooldown, InstanceLimit, MissingClip, Invalid, Paused
    }

    public readonly record struct AudioEventParameters(float Volume = 1, float Pitch = 1, string? Bus = null,
                                                        byte? Priority = null)
    {
        public AudioEventParameters() : this(1, 1, null, null) { }
    }

    public readonly struct AudioVoiceHandle
    {
        private readonly ulong m_Engine;
        private readonly uint m_Index, m_Generation;

        internal AudioVoiceHandle(ulong engine, uint index, uint generation)
        {
            m_Engine = engine;
            m_Index = index;
            m_Generation = generation;
        }

        /// <summary>Whether a voice was returned. A voice can finish or be stolen after it starts.</summary>
        public bool IsValid => m_Generation != 0;

        public bool Stop(float fadeSeconds = 0) => AudioCalls.Send(0, AudioOperation.StopVoice,
            new AudioCommandData { Engine = m_Engine, VoiceIndex = m_Index, VoiceGeneration = m_Generation,
                                   Value = fadeSeconds }) != 0;
    }

    public readonly record struct AudioEventTriggerResult(AudioEventTriggerStatus Status, AudioVoiceHandle Voice);

    public enum SpeakerMode { Auto, Mono, Stereo, Surround51, Surround71 }
    public enum AudioSpatializer { Panning, HRTF }
    public enum AudioOcclusionMode { Off, Raycast, Volumetric }
    public enum AudioTransmissionMode { Off, FrequencyIndependent, FrequencyDependent }
    public enum AudioDeviceChangeReason { Selected, Rerouted, FellBackToDefault, SpeakerModeChanged }
    public readonly record struct AudioOutputFormat(int Channels, ulong PackedChannelMap)
    {
        /// <summary>Returns the miniaudio channel position at an output index, or zero if unknown.</summary>
        public byte GetChannelPosition(int index) =>
            index >= 0 && index < Channels && index < 8 ? (byte)(PackedChannelMap >> (index * 8)) : (byte)0;
    }
    public readonly record struct AudioOutputDevice(string Id, string Name, bool IsDefault, AudioOutputFormat NativeFormat);
    public readonly record struct AudioDeviceChange(string DeviceId, AudioDeviceChangeReason Reason, AudioOutputFormat Format);

    public readonly record struct AudioProfilingStats(bool Enabled, float MixerLoadPercent, float AverageMixerLoadPercent,
        float PeakMixerLoadPercent, uint RealVoices, uint VirtualVoices, ulong Underruns, ulong UnderrunFrames);

    public static class Audio
    {
        public static class Profiling
        {
            public static bool Enabled => GetStats().Enabled;

            public static AudioProfilingStats GetStats()
            {
                var data = AudioCalls.Read(0, AudioOperation.ProfileStats);
                return new AudioProfilingStats(data.Flags != 0, data.Value, data.Volume, data.Pitch,
                    data.VoiceIndex, data.VoiceGeneration, data.Source, data.Local);
            }
        }

        public static event Action<AudioDeviceChange>? DeviceChanged;

        /// <summary>The active SOFA asset. An empty handle selects the built-in HRTF.</summary>
        public static AssetHandle ActiveHRTF
        {
            get
            {
                var data = AudioCalls.Read(0, AudioOperation.ActiveHRTF);
                return new AssetHandle(data.Source, data.Local);
            }
            set => SetActiveHRTF(value);
        }

        public static bool SetActiveHRTF(AssetHandle asset) => AudioCalls.Send(0, AudioOperation.ActiveHRTF,
            new AudioCommandData { Source = asset.Source, Local = asset.Local, Flags = AudioCalls.Write }) != 0;

        public static SpeakerMode SpeakerMode
        {
            get => (SpeakerMode)AudioCalls.Read(0, AudioOperation.SpeakerMode).Priority;
            set => SetSpeakerMode(value);
        }

        public static bool SetSpeakerMode(SpeakerMode mode) => AudioCalls.Send(0, AudioOperation.SpeakerMode,
            new AudioCommandData { Priority = (int)mode, Flags = AudioCalls.Write }) != 0;

        public static string OutputDeviceId => AudioCalls.ReadText(AudioCalls.Read(0, AudioOperation.SelectDevice));
        public static bool SetOutputDevice(string? id = null) => AudioCalls.Send(0, AudioOperation.SelectDevice,
            new AudioCommandData { Flags = AudioCalls.Write }, id ?? "default") != 0;

        public static IReadOnlyList<AudioOutputDevice> EnumerateOutputDevices() => AudioCalls.EnumerateDevices();
        public static bool SaveUserSettings() => AudioCalls.Send(0, AudioOperation.SaveUserSettings) != 0;
        public static bool LoadUserSettings() => AudioCalls.Send(0, AudioOperation.LoadUserSettings) != 0;

        private static ulong s_DeviceRevision, s_OutputEngine;

        internal static void DispatchDeviceChange()
        {
            var callback = DeviceChanged;
            if (callback == null || !AudioCalls.ReadDeviceChange(ref s_OutputEngine, ref s_DeviceRevision, out var change))
                return;
            foreach (Action<AudioDeviceChange> subscriber in callback.GetInvocationList())
            {
                try { subscriber(change); }
                catch (Exception error) { Log.Error(error.ToString()); }
            }
        }

        internal static void ResetDeviceEvents()
        {
            DeviceChanged = null;
            s_DeviceRevision = s_OutputEngine = 0;
        }

        public static AudioEventTriggerResult PlayOneShot(AssetHandle clipOrEvent, Vector3 position, float volume = 1) =>
            AudioCalls.Trigger(0, false, AudioOperation.PlayOneShot, clipOrEvent, position, new AudioEventParameters(volume));

        public static AudioEventTriggerResult TriggerEvent(AssetHandle evt, Vector3 position,
                                                            AudioEventParameters? parameters = null) =>
            AudioCalls.Trigger(0, false, AudioOperation.TriggerEvent, evt, position, parameters ?? new AudioEventParameters());

        public static AudioEventTriggerResult TriggerEvent(AssetHandle evt, Entity entity,
                                                            AudioEventParameters? parameters = null) =>
            AudioCalls.Trigger(entity?.ID ?? 0, true, AudioOperation.TriggerEvent, evt, default,
                               parameters ?? new AudioEventParameters());
    }

    public sealed class AudioSource : Component
    {
        public unsafe int PushGeneratorFrames(ReadOnlySpan<float> interleaved)
        {
            fixed (float* pcm = interleaved)
                return AudioCalls.Send(Entity.ID, AudioOperation.PushGenerator,
                    new AudioCommandData { Samples = pcm, SampleCount = interleaved.Length });
        }

        public int GetFramesAvailableToPush() => AudioCalls.Send(Entity.ID, AudioOperation.GeneratorRoom);
        /// <summary>Number of missing input frames replaced by silence by this generator voice.</summary>
        public ulong GeneratorUnderrunFrames => AudioCalls.Read(Entity.ID, AudioOperation.GeneratorUnderruns).Local;

        public bool Play() => AudioCalls.Send(Entity.ID, AudioOperation.Play) != 0;
        public bool Stop() => AudioCalls.Send(Entity.ID, AudioOperation.Stop) != 0;
        public bool Pause() => AudioCalls.Send(Entity.ID, AudioOperation.Pause) != 0;
        public bool UnPause() => AudioCalls.Send(Entity.ID, AudioOperation.UnPause) != 0;
        public bool IsPlaying => AudioCalls.Send(Entity.ID, AudioOperation.IsPlaying) != 0;
        public AudioEventTriggerResult PlayOneShot(AssetHandle handle, float volume = 1) =>
            AudioCalls.Trigger(Entity.ID, true, AudioOperation.PlayOneShot, handle, default, new AudioEventParameters(volume));

        public float Volume { get => ReadNumber(AudioOperation.Volume); set => WriteNumber(AudioOperation.Volume, value); }
        public float Pitch { get => ReadNumber(AudioOperation.Pitch); set => WriteNumber(AudioOperation.Pitch, value); }
        public AudioSpatializer Spatializer
        {
            get => (AudioSpatializer)AudioCalls.Read(Entity.ID, AudioOperation.Spatializer).Priority;
            set => AudioCalls.Send(Entity.ID, AudioOperation.Spatializer,
                new AudioCommandData { Priority = (int)value, Flags = AudioCalls.Write });
        }
        public float SpatialBlend
        {
            get => ReadNumber(AudioOperation.SpatialBlend);
            set => WriteNumber(AudioOperation.SpatialBlend, value);
        }
        public AudioOcclusionMode Occlusion
        {
            get => (AudioOcclusionMode)AudioCalls.Read(Entity.ID, AudioOperation.Occlusion).Priority;
            set => AudioCalls.Send(Entity.ID, AudioOperation.Occlusion,
                new AudioCommandData { Priority = (int)value, Flags = AudioCalls.Write });
        }
        public AudioTransmissionMode Transmission
        {
            get => (AudioTransmissionMode)AudioCalls.Read(Entity.ID, AudioOperation.Transmission).Priority;
            set => AudioCalls.Send(Entity.ID, AudioOperation.Transmission,
                new AudioCommandData { Priority = (int)value, Flags = AudioCalls.Write });
        }
        public float OcclusionStrength
        {
            get => ReadNumber(AudioOperation.OcclusionStrength);
            set => WriteNumber(AudioOperation.OcclusionStrength, value);
        }
        public float TransmissionStrength
        {
            get => ReadNumber(AudioOperation.TransmissionStrength);
            set => WriteNumber(AudioOperation.TransmissionStrength, value);
        }
        public uint OcclusionLayerMask
        {
            get => (uint)AudioCalls.Read(Entity.ID, AudioOperation.OcclusionLayerMask).Local;
            set => AudioCalls.Send(Entity.ID, AudioOperation.OcclusionLayerMask,
                new AudioCommandData { Local = value, Flags = AudioCalls.Write });
        }
        public float OcclusionRadius
        {
            get => ReadNumber(AudioOperation.OcclusionRadius);
            set => WriteNumber(AudioOperation.OcclusionRadius, value);
        }
        public uint OcclusionSamples
        {
            get => (uint)AudioCalls.Read(Entity.ID, AudioOperation.OcclusionSamples).Local;
            set => AudioCalls.Send(Entity.ID, AudioOperation.OcclusionSamples,
                new AudioCommandData { Local = value, Flags = AudioCalls.Write });
        }
        /// <summary>Last measured strength-scaled obstruction, from zero (clear) to one (fully blocked).</summary>
        public float CurrentOcclusion => ReadNumber(AudioOperation.CurrentOcclusion);
        public bool Loop { get => ReadNumber(AudioOperation.Loop) != 0; set => WriteNumber(AudioOperation.Loop, value ? 1 : 0); }
        /// <summary>Source playback position in seconds.</summary>
        public float Time { get => ReadNumber(AudioOperation.Time); set => WriteNumber(AudioOperation.Time, value); }
        public AssetHandle Clip { get => ReadAsset(AudioOperation.Clip); set => WriteAsset(AudioOperation.Clip, value); }
        /// <summary>When set, Play uses this event in place of Clip.</summary>
        public AssetHandle Event { get => ReadAsset(AudioOperation.Event); set => WriteAsset(AudioOperation.Event, value); }
        public string Bus
        {
            get => AudioCalls.ReadBus(Entity.ID);
            set => AudioCalls.Send(Entity.ID, AudioOperation.Bus, new AudioCommandData { Flags = AudioCalls.Write }, value ?? "");
        }

        private float ReadNumber(AudioOperation operation) => AudioCalls.Read(Entity.ID, operation).Value;
        private void WriteNumber(AudioOperation operation, float value) =>
            AudioCalls.Send(Entity.ID, operation, new AudioCommandData { Value = value, Flags = AudioCalls.Write });
        private AssetHandle ReadAsset(AudioOperation operation)
        {
            var data = AudioCalls.Read(Entity.ID, operation);
            return new AssetHandle(data.Source, data.Local);
        }
        private void WriteAsset(AudioOperation operation, AssetHandle value) =>
            AudioCalls.Send(Entity.ID, operation,
                new AudioCommandData { Source = value.Source, Local = value.Local, Flags = AudioCalls.Write });
    }

    public readonly record struct MusicSetPosition(string State, string Segment, ulong Bar, float Beat, float Seconds);
    public readonly record struct MusicSetBeat(ulong Sample, ulong Bar, uint Beat);
    public readonly record struct MusicSetCue(ulong Sample, string Name);

    public sealed class MusicSetPlayer : Component
    {
        private static MusicSetPlayer[] s_Listeners = Array.Empty<MusicSetPlayer>();
        private Action<MusicSetBeat>? m_Beat, m_Bar;
        private Action<MusicSetCue>? m_Cue, m_State;
        private bool m_Listening;
        private List<(byte[] Bytes, string Text)>? m_Names;
        private List<(byte[] Bytes, string Text)> Names => m_Names ??= new();

        public bool Play(float fadeSeconds = 0) => AudioCalls.Send(Entity.ID, AudioOperation.MusicPlay,
            new AudioCommandData { Value = fadeSeconds }) != 0;
        public bool Stop(float fadeSeconds = 0) => AudioCalls.Send(Entity.ID, AudioOperation.MusicStop,
            new AudioCommandData { Value = fadeSeconds }) != 0;
        public bool SetState(string name) => AudioCalls.Send(Entity.ID, AudioOperation.MusicSetState, bus: name ?? "") != 0;
        public bool SetParameter(string name, float value) => AudioCalls.Send(Entity.ID, AudioOperation.MusicSetParameter,
            new AudioCommandData { Value = value }, name ?? "") != 0;
        public bool TriggerStinger(string name) => AudioCalls.Send(Entity.ID, AudioOperation.MusicStinger, bus: name ?? "") != 0;
        public MusicSetPosition? Position => AudioCalls.ReadMusicPosition(Entity.ID, Names);
        public string CurrentState => Position?.State ?? "";
        public string CurrentSegment => Position?.Segment ?? "";
        public ulong Bar => Position?.Bar ?? 0;
        public float BeatInBar => Position?.Beat ?? 0;

        public event Action<MusicSetBeat> Beat
        {
            add { m_Beat += value; SyncListener(); }
            remove { m_Beat -= value; SyncListener(); }
        }
        public event Action<MusicSetBeat> BarStarted
        {
            add { m_Bar += value; SyncListener(); }
            remove { m_Bar -= value; SyncListener(); }
        }
        public event Action<MusicSetCue> Cue
        {
            add { m_Cue += value; SyncListener(); }
            remove { m_Cue -= value; SyncListener(); }
        }
        public event Action<MusicSetCue> StateChanged
        {
            add { m_State += value; SyncListener(); }
            remove { m_State -= value; SyncListener(); }
        }

        private void SyncListener()
        {
            bool listen = m_Beat != null || m_Bar != null || m_Cue != null || m_State != null;
            if (listen == m_Listening) return;
            m_Listening = listen;
            AudioCalls.Send(Entity.ID, AudioOperation.MusicListen, new AudioCommandData { Flags = listen ? 1 : 0 });
            if (listen)
            {
                var listeners = s_Listeners;
                s_Listeners = new MusicSetPlayer[listeners.Length + 1];
                listeners.CopyTo(s_Listeners, 0);
                s_Listeners[listeners.Length] = this;
            }
            else
            {
                var listeners = s_Listeners;
                int index = Array.IndexOf(listeners, this);
                if (index < 0) return;
                s_Listeners = new MusicSetPlayer[listeners.Length - 1];
                Array.Copy(listeners, 0, s_Listeners, 0, index);
                Array.Copy(listeners, index + 1, s_Listeners, index, listeners.Length - index - 1);
            }
        }

        internal static void DispatchEvents()
        {
            foreach (var listener in s_Listeners)
                while (listener.m_Listening && AudioCalls.ReadMusicEvent(listener.Entity.ID, out var data))
                {
                    try
                    {
                        switch (data.Priority)
                        {
                            case 0: listener.m_Beat?.Invoke(new MusicSetBeat(data.Source, data.Local, data.VoiceIndex)); break;
                            case 1: listener.m_Bar?.Invoke(new MusicSetBeat(data.Source, data.Local, data.VoiceIndex)); break;
                            case 2: listener.m_Cue?.Invoke(new MusicSetCue(data.Source, AudioCalls.ReadMusicName(data, listener.Names))); break;
                            case 3: listener.m_State?.Invoke(new MusicSetCue(data.Source, AudioCalls.ReadMusicName(data, listener.Names))); break;
                        }
                    }
                    catch (Exception error) { Log.Error(error.ToString()); }
                }
        }

        internal static void ResetEvents()
        {
            foreach (var listener in s_Listeners)
            {
                listener.m_Listening = false;
                listener.m_Beat = listener.m_Bar = null;
                listener.m_Cue = listener.m_State = null;
            }
            s_Listeners = Array.Empty<MusicSetPlayer>();
        }
    }

    public static class AudioBus
    {
        public static float? GetUserVolumeDb(string bus) => AudioCalls.ReadBusSetting(AudioOperation.UserBusVolume, bus ?? "");
        public static bool? GetUserMuted(string bus) => AudioCalls.ReadBusSetting(AudioOperation.UserBusMute, bus ?? "") is float value
            ? value != 0 : null;
        public static bool SetUserVolumeDb(string bus, float db) => AudioCalls.Send(0, AudioOperation.UserBusVolume,
            new AudioCommandData { Value = db, Flags = AudioCalls.Write }, bus ?? "") != 0;
        public static bool SetUserMuted(string bus, bool muted) => AudioCalls.Send(0, AudioOperation.UserBusMute,
            new AudioCommandData { Value = muted ? 1 : 0, Flags = AudioCalls.Write }, bus ?? "") != 0;

        public static bool SetVolumeDb(string bus, float db) => AudioCalls.Send(0, AudioOperation.BusVolume,
            new AudioCommandData { Value = db, Flags = AudioCalls.Write }, bus ?? "") != 0;

        public static float? GetVolumeDb(string bus) => AudioCalls.ReadBusSetting(AudioOperation.BusVolume, bus ?? "");

        public static bool SetMuted(string bus, bool muted) => AudioCalls.Send(0, AudioOperation.BusMute,
            new AudioCommandData { Value = muted ? 1 : 0 }, bus ?? "") != 0;
    }

    internal enum AudioOperation
    {
        Play, Stop, Pause, UnPause, IsPlaying,
        Volume, Pitch, Loop, Clip, Event, Bus, Time,
        PlayOneShot, TriggerEvent, StopVoice, BusVolume, BusMute,
        UserBusVolume, UserBusMute, SaveUserSettings, LoadUserSettings, SpeakerMode, SelectDevice,
        EnumerateDevices, ReadDevice, LastDeviceChange, PushGenerator, GeneratorRoom, GeneratorUnderruns, ProfileStats,
        Spatializer, SpatialBlend, ActiveHRTF,
        Occlusion, Transmission, OcclusionStrength, TransmissionStrength, OcclusionLayerMask,
        OcclusionRadius, OcclusionSamples, CurrentOcclusion,
        MusicPlay, MusicStop, MusicSetState, MusicSetParameter, MusicStinger, MusicPosition, MusicListen, MusicEvent
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct AudioCommandData
    {
        internal ulong Source, Local, Engine;
        internal uint VoiceIndex, VoiceGeneration;
        internal Vector3 Position;
        internal float Volume, Pitch, Value;
        internal int Priority, Flags;
        internal byte* Bus;
        internal int BusLength;
        internal byte* DeviceName;
        internal int DeviceNameLength;
        internal float* Samples;
        internal int SampleCount;
    }

    internal static unsafe class AudioCalls
    {
        internal const int Write = 1, EntityTarget = 2;

        private static int Call(ulong entity, AudioOperation operation, AudioCommandData* data, string? bus = null)
        {
            if (bus == null)
                return InternalCalls.Api.AudioCommand(entity, (int)operation, data);
            int length = Encoding.UTF8.GetByteCount(bus);
            Span<byte> bytes = length < 256 ? stackalloc byte[length + 1] : new byte[length + 1];
            Encoding.UTF8.GetBytes(bus.AsSpan(), bytes);
            fixed (byte* text = bytes)
            {
                data->Bus = text;
                data->BusLength = length;
                int result = InternalCalls.Api.AudioCommand(entity, (int)operation, data);
                data->Bus = null;
                return result;
            }
        }

        internal static AudioCommandData Read(ulong entity, AudioOperation operation)
        {
            AudioCommandData data = default;
            Call(entity, operation, &data);
            return data;
        }

        internal static int Send(ulong entity, AudioOperation operation, AudioCommandData data = default, string? bus = null) =>
            Call(entity, operation, &data, bus);

        internal static MusicSetPosition? ReadMusicPosition(ulong entity, List<(byte[] Bytes, string Text)> names)
        {
            AudioCommandData data = default;
            if (Call(entity, AudioOperation.MusicPosition, &data) == 0) return null;
            return new MusicSetPosition(ReadMusicName(data, names),
                CachedName(new ReadOnlySpan<byte>(data.DeviceName, data.DeviceNameLength), names),
                data.Source, data.Value, data.Pitch);
        }

        internal static string ReadMusicName(AudioCommandData data, List<(byte[] Bytes, string Text)> names) =>
            CachedName(new ReadOnlySpan<byte>(data.Bus, data.BusLength), names);

        private static string CachedName(ReadOnlySpan<byte> bytes, List<(byte[] Bytes, string Text)> names)
        {
            foreach (var name in names)
                if (bytes.SequenceEqual(name.Bytes)) return name.Text;
            string text = Encoding.UTF8.GetString(bytes);
            names.Add((bytes.ToArray(), text));
            return text;
        }

        internal static bool ReadMusicEvent(ulong entity, out AudioCommandData data)
        {
            AudioCommandData result = default;
            bool success = Call(entity, AudioOperation.MusicEvent, &result) != 0;
            data = result;
            return success;
        }

        internal static string ReadBus(ulong entity)
        {
            var data = Read(entity, AudioOperation.Bus);
            return data.Bus != null ? Encoding.UTF8.GetString(data.Bus, data.BusLength) : "";
        }

        internal static string ReadText(AudioCommandData data) =>
            data.Bus != null ? Encoding.UTF8.GetString(data.Bus, data.BusLength) : "";

        internal static IReadOnlyList<AudioOutputDevice> EnumerateDevices()
        {
            int count = Send(0, AudioOperation.EnumerateDevices);
            if (count <= 0) return Array.Empty<AudioOutputDevice>();
            var devices = new AudioOutputDevice[count];
            for (int index = 0; index < count; ++index)
            {
                AudioCommandData data = new() { Priority = index };
                if (Call(0, AudioOperation.ReadDevice, &data) != 0)
                    devices[index] = new AudioOutputDevice(ReadText(data),
                        Encoding.UTF8.GetString(data.DeviceName, data.DeviceNameLength), data.Flags != 0,
                        new AudioOutputFormat((int)data.Source, data.Local));
            }
            return devices;
        }

        internal static bool ReadDeviceChange(ref ulong engine, ref ulong revision, out AudioDeviceChange change)
        {
            AudioCommandData data = new() { Engine = engine, VoiceIndex = (uint)revision, VoiceGeneration = (uint)(revision >> 32) };
            change = default;
            if (Call(0, AudioOperation.LastDeviceChange, &data) == 0)
                return false;
            revision = ((ulong)data.VoiceGeneration << 32) | data.VoiceIndex;
            engine = data.Engine;
            change = new AudioDeviceChange(ReadText(data), (AudioDeviceChangeReason)data.Priority,
                new AudioOutputFormat((int)data.Source, data.Local));
            return true;
        }

        internal static float? ReadBusSetting(AudioOperation operation, string bus)
        {
            AudioCommandData data = default;
            return Call(0, operation, &data, bus) != 0 ? data.Value : null;
        }

        internal static AudioEventTriggerResult Trigger(ulong entity, bool entityTarget, AudioOperation operation,
            AssetHandle asset, Vector3 position, AudioEventParameters parameters)
        {
            AudioCommandData data = new()
            {
                Source = asset.Source, Local = asset.Local, Position = position,
                Volume = parameters.Volume, Pitch = parameters.Pitch, Priority = (int?)parameters.Priority ?? -1,
                Flags = entityTarget ? EntityTarget : 0
            };
            var status = (AudioEventTriggerStatus)Call(entity, operation, &data, parameters.Bus);
            return new AudioEventTriggerResult(status, new AudioVoiceHandle(data.Engine, data.VoiceIndex, data.VoiceGeneration));
        }
    }
}
