using System;
using System.Globalization;
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

    public static class Audio
    {
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
        public bool Play() => AudioCalls.Send(Entity.ID, AudioOperation.Play) != 0;
        public bool Stop() => AudioCalls.Send(Entity.ID, AudioOperation.Stop) != 0;
        public bool Pause() => AudioCalls.Send(Entity.ID, AudioOperation.Pause) != 0;
        public bool UnPause() => AudioCalls.Send(Entity.ID, AudioOperation.UnPause) != 0;
        public bool IsPlaying => AudioCalls.Send(Entity.ID, AudioOperation.IsPlaying) != 0;
        public AudioEventTriggerResult PlayOneShot(AssetHandle handle, float volume = 1) =>
            AudioCalls.Trigger(Entity.ID, true, AudioOperation.PlayOneShot, handle, default, new AudioEventParameters(volume));

        public float Volume { get => ReadNumber(AudioOperation.Volume); set => WriteNumber(AudioOperation.Volume, value); }
        public float Pitch { get => ReadNumber(AudioOperation.Pitch); set => WriteNumber(AudioOperation.Pitch, value); }
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

    public static class AudioBus
    {
        public static bool SetVolumeDb(string bus, float db) => AudioCalls.Send(0, AudioOperation.BusVolume,
            new AudioCommandData { Value = db, Flags = AudioCalls.Write }, bus ?? "") != 0;

        public static float? GetVolumeDb(string bus) => AudioCalls.ReadBusVolume(bus ?? "");

        public static bool SetMuted(string bus, bool muted) => AudioCalls.Send(0, AudioOperation.BusMute,
            new AudioCommandData { Value = muted ? 1 : 0 }, bus ?? "") != 0;
    }

    internal enum AudioOperation
    {
        Play, Stop, Pause, UnPause, IsPlaying,
        Volume, Pitch, Loop, Clip, Event, Bus, Time,
        PlayOneShot, TriggerEvent, StopVoice, BusVolume, BusMute
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

        internal static string ReadBus(ulong entity)
        {
            var data = Read(entity, AudioOperation.Bus);
            return data.Bus != null ? Encoding.UTF8.GetString(data.Bus, data.BusLength) : "";
        }

        internal static float? ReadBusVolume(string bus)
        {
            AudioCommandData data = default;
            return Call(0, AudioOperation.BusVolume, &data, bus) != 0 ? data.Value : null;
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
